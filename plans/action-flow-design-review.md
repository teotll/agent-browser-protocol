# ABP Action Flow: Holistic Design Review

## Context

The virtual cursor moves to the correct position on screen but does NOT appear at the correct position in action screenshots. Specifically, a `browser_mouse_move(400, 400)` MCP call returns before/after screenshots both showing the cursor at the old (100,100) position, but the cursor visibly jumps to (400,400) on the physical screen after the response is sent.

This document maps out how the action flow interleaves with JavaScript debugger pause/resume, virtual time, virtual cursor positioning, and screenshot capture — and identifies timing issues.

## Current Flow (Click/Move Action)

```
STEP  ACTION                          JS STATE   VTIME STATE   CURSOR STATE
─────────────────────────────────────────────────────────────────────────────
 0    Entry (from idle)               PAUSED     PAUSED        Previous pos
 1    Start() — validate, get CDP     PAUSED     PAUSED        Previous pos
 2    ResumeExecution()               PAUSED→RUN PAUSED→RUN    Previous pos
      ├─ Debugger.resume (CDP)
      ├─ Emulation.setVirtualTimePolicy("realtime") (CDP)
      └─ Page.bringToFront (CDP)     [kicks compositor]
 3    CaptureBeforeScreenshot()       RUNNING    RUNNING       Previous pos
      ├─ [optional] Runtime.evaluate to inject markup CSS
      ├─ ForceRedraw (Mojo, associated pipe)
      ├─ 167ms GPU wait
      └─ GrabViewSnapshot (OS ScreenCaptureKit)
 4    ExecuteAction() — action callback RUNNING  RUNNING       Previous pos
 5    Action Lambda:                  RUNNING    RUNNING       ► SET NEW POS
      ├─ UpdateVirtualCursorState()   [bookkeeping: tab_state.cursor = (400,400)]
      ├─ SetVirtualCursorEnabledViaMojo() → Mojo associated pipe
      ├─ SetVirtualCursorViaMojo()    → Mojo associated pipe [ASYNC - queued]
      ├─ Input.dispatchMouseEvent     → CDP [ASYNC - queued]
      └─ OnActionDispatched()
 6    DoWaitUntil()                   RUNNING    RUNNING       New pos (maybe composited)
      └─ Wait 500ms + page load/DOMContentLoaded/firstPaint
         [risk: auto-pause can fire during wait, freezing JS+vtime]
 7    StopEventCaptureAndGetScroll    RUNNING    RUNNING       New pos
      └─ Runtime.evaluate(scrollX/Y)  → CDP
 8    EnsureVirtualCursorVisible()    RUNNING    RUNNING       RE-SEND new pos
      ├─ SetVirtualCursorViaMojo()    → Mojo associated pipe [re-send (400,400)]
      ├─ InsertVisualStateCallback    → Mojo NON-associated pipe ← RACE!
      └─ 500ms timeout fallback
 9    CaptureAfterScreenshot()        RUNNING    RUNNING       ← SHOULD be composited
      ├─ [optional] Runtime.evaluate to inject markup CSS
      ├─ ForceRedraw (Mojo, associated pipe)
      ├─ 167ms GPU wait
      └─ GrabViewSnapshot (OS ScreenCaptureKit)
10    PauseExecution()                RUN→PAUSE  RUN→PAUSE     Composited
      ├─ Emulation.setVirtualTimePolicy("pause") (CDP)
      └─ Debugger.pause (CDP)
11    SendResponse()                  PAUSED     PAUSED        At new pos
```

## Mojo Pipe Architecture (Critical for Understanding Ordering)

```
Browser Process                    Renderer Process
──────────────                    ────────────────
                                  Main Thread
blink_frame_widget_ ─── ASSOCIATED ──→ WebFrameWidgetImpl (FrameWidget)
blink_widget_ ────────── ASSOCIATED ──→ WidgetBase (Widget)
virtual_cursor_remote_ ─ ASSOCIATED ──→ WebFrameWidgetImpl (VirtualCursor)

widget_compositor_ ──── NON-ASSOCIATED → WidgetBase (WidgetCompositor)
                                         ↓
                                  Compositor Thread
```

**Key ordering guarantee**: Associated pipes are ordered relative to each other. So:
- `SetPosition(400,400)` via `virtual_cursor_remote_` (associated)
- `ForceRedraw()` via `blink_widget_` (associated)
- → **ForceRedraw is guaranteed to run AFTER SetPosition on the renderer**

**Key NON-guarantee**: `InsertVisualStateCallback` uses `widget_compositor_` (non-associated). It can fire BEFORE `SetPosition` is processed, because it takes a different path (compositor thread → swap promise → callback).

## Identified Issues

### Issue 1: InsertVisualStateCallback Race (Root Cause of Screenshot Bug)

`EnsureVirtualCursorVisible()` does:
1. `SetVirtualCursorViaMojo(400, 400)` — associated pipe
2. `InsertVisualStateCallback` — **non-associated** pipe

The non-associated `VisualStateRequest` can arrive and complete BEFORE `SetPosition` is processed by the renderer main thread. When this happens:
- The compositor produces a frame WITHOUT the cursor at (400,400)
- The swap promise fires, callback returns to browser
- `CaptureAfterScreenshot()` starts immediately
- `ForceRedraw()` is sent on the associated pipe (guaranteed after SetPosition)

**So ForceRedraw SHOULD fix it** — it forces a new BeginMainFrame that includes the cursor update. BUT there's a problem: if `ForceRedraw` itself has timing issues (timeout, blink_widget_ temporarily null), the fallback path (`GrabViewSnapshot` without ForceRedraw) captures a stale frame without the cursor.

**More critically**: Even when ForceRedraw succeeds, the `GrabViewSnapshot` after the 167ms GPU wait may capture the WRONG frame if the display hasn't been updated yet or if ScreenCaptureKit's capture pipeline has latency.

### Issue 2: InsertVisualStateCallback is Unnecessary

The `InsertVisualStateCallback` step in `EnsureVirtualCursorVisible` was intended to "guarantee the cursor Mojo message has been composited." But:
1. It uses a non-associated pipe, so it does NOT guarantee cursor message ordering
2. The subsequent `ForceRedraw` (associated pipe) already provides a stronger guarantee
3. The 500ms timeout adds unnecessary delay risk

`InsertVisualStateCallback` adds complexity and a false sense of security without providing the guarantee it claims.

### Issue 3: Double ForceRedraw Inefficiency

The flow does:
1. `InsertVisualStateCallback` (may trigger a compositor frame)
2. Then `ForceRedraw` in `CaptureActionScreenshot` (forces another frame)
3. Then 167ms GPU wait
4. Then `GrabViewSnapshot`

This is two compositor roundtrips where one would suffice.

### Issue 4: ForceRedraw Timeout Fallback Loses Cursor

If `ForceRedraw` times out (500ms × 2 = 1 second), the fallback is:
```cpp
ui::GrabViewSnapshot(view, gfx::Rect(view_size), callback);
```
This captures whatever is currently on the OS screen buffer — which may not have the cursor at the new position if the compositor hasn't produced a frame yet. The cursor update is lost in the screenshot.

### Issue 5: GrabViewSnapshot OS-Level Latency

`GrabViewSnapshot` uses macOS ScreenCaptureKit. There is inherent latency between when the GPU presents a frame and when ScreenCaptureKit can capture it. The 167ms "GPU wait" heuristic may not always be sufficient, especially under load.

## User's Desired Timeline

```
1. Before screenshot taken
2. Debug and virtual time unpaused
3. ABP action inputted (cursor updated in both viewport & tracking)
4. Wait for load conditions + at least 500ms
5. After screenshot taken (shows cursor at new position)
```

## Assessment: Feasibility & Chrome Primitive Clashes

**The desired timeline IS feasible.** It's essentially what the current design does. The issue is not with the ordering but with the **compositor synchronization** between cursor update and screenshot capture.

### Chrome Primitive Constraints

1. **Mojo IPC is asynchronous**: `SetPosition` is queued; the renderer processes it later. You cannot synchronously confirm the cursor is painted.

2. **`InsertVisualStateCallback` uses a non-associated pipe**: It does NOT provide ordering guarantees with respect to cursor Mojo messages on the associated pipe. It's the wrong synchronization primitive for this use case.

3. **`ForceRedraw` provides the right guarantee**: It's on the associated pipe (ordered after cursor messages). It forces `BeginMainFrame` → paint → composite → present. The cursor IS painted in this frame.

4. **`GrabViewSnapshot` has inherent latency**: It captures the OS display buffer, not the compositor surface directly. The 167ms GPU wait is a heuristic, not a guarantee.

5. **Compositor only produces frames while virtual time is running**: Screenshots MUST be taken before pausing virtual time, which the current design correctly does.

## Proposed Fix

### Simplify EnsureVirtualCursorVisible → Remove InsertVisualStateCallback

Replace the current `EnsureVirtualCursorVisible()` with a simpler approach that relies on `ForceRedraw` (the associated pipe guarantee) instead of `InsertVisualStateCallback` (the non-associated pipe race):

```
Current flow:
  EnsureVirtualCursorVisible:
    SetPosition (associated) → InsertVisualStateCallback (non-associated) → [wait] →
    CaptureAfterScreenshot → ForceRedraw (associated) → GrabViewSnapshot

Proposed flow:
  EnsureVirtualCursorVisible:
    SetPosition (associated) → CaptureAfterScreenshot → ForceRedraw (associated) → GrabViewSnapshot
```

The `ForceRedraw` in `CaptureActionScreenshot` already provides the compositor synchronization guarantee we need. `InsertVisualStateCallback` is redundant and introduces a race condition.

### Changes

**File: `chrome/browser/abp/abp_action_context.cc`**

Simplify `EnsureVirtualCursorVisible()`:
- Keep the `SetVirtualCursorEnabledViaMojo` and `SetVirtualCursorViaMojo` calls (these set the cursor on the associated pipe)
- Remove `InsertVisualStateCallback`, `OnVisualStateCallbackFired`, `OnVisualStateTimeout`, and the `visual_state_completed_` flag
- Go directly to `CaptureAfterScreenshot()` — the `ForceRedraw` inside `CaptureActionScreenshot` is on the associated pipe and is guaranteed to be processed AFTER `SetPosition`

**File: `chrome/browser/abp/abp_action_context.h`**

Remove `OnVisualStateCallbackFired`, `OnVisualStateTimeout`, and `visual_state_completed_` declarations.

### Why This Works

1. `SetVirtualCursorViaMojo(400, 400)` sends `SetPosition` on the **associated** Mojo pipe
2. `CaptureActionScreenshot` calls `GetSnapshotFromBrowser(from_surface=false)` which calls `ForceRedraw` on the **associated** Mojo pipe via `blink_widget_`
3. Associated pipe ordering guarantees `SetPosition` is processed BEFORE `ForceRedraw`
4. `ForceRedraw` forces `BeginMainFrame` → paint (with cursor at 400,400) → composite → present
5. After 167ms GPU wait, `GrabViewSnapshot` captures the frame with cursor at correct position

No `InsertVisualStateCallback` race, no non-associated pipe ordering issues, no redundant compositor frames.

### Edge Case: ForceRedraw Timeout

If `ForceRedraw` times out (blink_widget_ null during navigation), the retry/fallback mechanism in `CaptureActionScreenshotWithRetry` handles it. On second timeout, it falls back to direct `GrabViewSnapshot`. In this edge case, the cursor may not be at the correct position, but this only happens during cross-process navigation where the renderer is being swapped — an inherently unstable state where screenshot quality is already degraded.

## Verification

1. Build with `autoninja -C out/Default chrome`
2. Launch: `./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run`
3. Navigate to example.com
4. Move cursor to (100, 100) via REST API — verify after screenshot shows cursor at (100, 100)
5. Move cursor to (400, 400) via MCP — verify after screenshot shows cursor at (400, 400)
6. Move cursor to (800, 600) via MCP — verify cursor moves in after screenshot
7. Click at (200, 200) — verify cursor shows at click position in after screenshot
8. Navigate to a new page — verify cursor re-centers correctly
