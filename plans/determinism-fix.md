# Fix Virtual Cursor Screenshot Race & Deterministic Debugger Pause

## Context

Three determinism gaps exist in ABP's action lifecycle:

1. **Virtual cursor race**: `SetVirtualCursorPosition()` and `InsertVisualStateCallback()` go through **different Mojo pipes**. The cursor position may not be painted in the screenshot.

2. **Non-deterministic Debugger.pause**: `Debugger.pause` only sets a "pause on next JS statement" flag. If no JS runs (virtual time is already paused), the debugger never actually enters paused state.

3. **Fragile blink_widget_ null recovery**: During cross-process navigation, `blink_widget_` is null. Current recovery is a blind 750ms timeout + retry. Should be event-driven.

### Root Cause Evidence — Pipe Topology

```
RenderWidgetHostImpl (Browser Process)
│
├── blink_widget_          AssociatedRemote<Widget>          ← ForceRedraw()
│   (associated endpoint on primary channel)
│
├── blink_frame_widget_    AssociatedRemote<FrameWidget>     ← BindVirtualCursor()
│   (associated endpoint on primary channel)
│   │
│   ├── virtual_cursor_remote_   AssociatedRemote<VirtualCursor>
│   │   (BindNewEndpointAndPassReceiver → SAME channel, ORDERED with Widget)
│   │
│   └── widget_compositor_       Remote<WidgetCompositor>
│       (BindNewPipeAndPassReceiver → NEW SEPARATE PIPE, NOT ordered)
│       └── VisualStateRequest()    ← InsertVisualStateCallback()
```

**Key**: `BindNewEndpointAndPassReceiver` = associated (ordered). `BindNewPipeAndPassReceiver` = separate pipe (unordered).

Both `Widget` and `FrameWidget` are associated endpoints created from the same params (lines 3567-3577 of `render_widget_host_impl.cc`), sharing the same underlying IPC channel. `WidgetCompositor` is a **separate pipe** — no ordering guarantee.

---

## Fix 1: Replace InsertVisualStateCallback with ForceRedraw + GrabViewSnapshot

### Problem

```
Current:
  SetVirtualCursorPosition(x,y) ─── associated pipe (FrameWidget) ───→ Main thread
  InsertVisualStateCallback()   ─── SEPARATE pipe (WidgetCompositor) → Compositor thread
                                     ↑ NO ordering guarantee
  CopyFromSurface()             ─── Can be silently dropped by DelegatedFrameHost
```

### Solution: ForceRedraw (ordering fence) → GrabViewSnapshot (OS capture)

```
Fixed:
  SetVirtualCursorPosition(x,y) ─── associated pipe (FrameWidget) ───→ Main thread (1st)
  ForceRedraw()                 ─── associated pipe (Widget) ─────────→ Main thread (2nd)
                                     ↓ Guaranteed ordered with VirtualCursor
                                     Forces layout + paint + commit + present
                                     Callback fires after frame presented to display server
  Wait 167ms                    ─── CoreAnimation composites to window server
  GrabViewSnapshot()            ─── ScreenCaptureKit reads window server buffer (reliable)
                                     If blank: retry after 167ms (up to 5 times)
```

**Why GrabViewSnapshot over CopyFromSurface**:
- CopyFromSurface callback can be **silently dropped** by DelegatedFrameHost when surface is invalid
- GrabViewSnapshot captures actual screen content via ScreenCaptureKit — never silently drops
- Chromium's own `GetSnapshotFromBrowser(from_surface=false)` already uses this exact pattern (ForceRedraw + 167ms delay + GrabViewSnapshot) — see `render_widget_host_impl.cc:3670`

**Why 167ms**: Chromium uses this delay (10 vsyncs at 60fps) between ForceRedraw presentation callback and GrabViewSnapshot to ensure CoreAnimation has composited the frame to the window server buffer. See `render_widget_host_impl.cc:3670-3680`:
```cpp
// On Mac, when using CoreAnimation, there is a delay between when content
// is drawn to the screen, and when the snapshot will actually pick up that
// content. Insert a manual delay of 1/6th of a second (to simulate 10
// frames at 60 fps) before actually taking the snapshot.
base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
    FROM_HERE, ..., base::Seconds(1. / 6));  // 166.67ms
```

### ForceRedraw callback mechanics

```
Browser:  blink_widget_->ForceRedraw(callback)               [associated pipe]
  ↓
Renderer:  WidgetBase::ForceRedraw()
  ├── RequestPresentationTimeForNextFrame(callback)           [registers for next present]
  └── SetNeedsCommitWithForcedRedraw()                        [forces full repaint]
  ↓
Compositor: BeginMainFrame → Layout → Paint → Commit → Activate → Present
  ↓
PresentationFeedback fires → Mojo response → Browser callback
  ↓  (frame is now in display server)
Wait 167ms (CoreAnimation compositing delay)
  ↓
GrabViewSnapshot → ScreenCaptureKit captures from window server
  ↓  (if blank: retry after 167ms, max 5 retries)
Encode to base64
```

### Freshness detection for GrabViewSnapshot

ScreenCaptureKit doesn't expose frame metadata. Detect stale captures via:
- **Blank image check**: A blank macOS window produces a characteristic small image (~8654 bytes for 2070x1672 webp). Check `image.IsEmpty()` or compare encoded size to known blank threshold.
- **Retry on blank**: If captured image is blank, wait another 167ms and retry (compositor may not have presented yet).
- **Max retries**: 5 (total budget: 835ms worst case after ForceRedraw callback).

### Widget-ready notification (robust blink_widget_ null handling)

When `blink_widget_` is null during cross-process navigation, instead of a blind 750ms timeout:

**`content/browser/renderer_host/render_widget_host_impl.h`** — Add:
```cpp
// Callbacks to fire when blink_widget_ is bound (after renderer swap).
// Follows the same pattern as pending_virtual_cursor_state_.
std::vector<base::OnceClosure> pending_on_widget_bound_callbacks_;

// Force a compositor redraw and call |callback| after frame presentation.
// If blink_widget_ is not bound, queues callback for when it becomes bound.
void ForceRedrawWithCallback(base::OnceClosure callback);
```

**`content/browser/renderer_host/render_widget_host_impl.cc`** — Implement:
```cpp
void RenderWidgetHostImpl::ForceRedrawWithCallback(base::OnceClosure callback) {
  if (!blink_widget_) {
    // Widget not bound yet (cross-process navigation in progress).
    // Queue for retry when BindWidgetInterfaces completes.
    pending_on_widget_bound_callbacks_.push_back(
        base::BindOnce(&RenderWidgetHostImpl::ForceRedrawWithCallback,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
    return;
  }
  blink_widget_->ForceRedraw(
      base::BindOnce([](base::OnceClosure cb) { std::move(cb).Run(); },
                     std::move(callback)));
}
```

In `BindWidgetInterfaces`, after `blink_widget_.Bind(...)` (line ~732):
```cpp
// Fire any pending ForceRedraw callbacks now that widget is bound.
std::vector<base::OnceClosure> pending = std::move(pending_on_widget_bound_callbacks_);
for (auto& cb : pending) {
  std::move(cb).Run();
}
```

**Behavior**:
- `blink_widget_` available → ForceRedraw fires immediately
- `blink_widget_` null → callback queued → `BindWidgetInterfaces` fires it when new renderer connects → ForceRedraw retries automatically
- No blind timer. Event-driven. Zero wasted latency.

### Changes to `CaptureActionScreenshotWithRetry`

Replace the InsertVisualStateCallback + CopyFromSurface flow with ForceRedraw + GrabViewSnapshot:

```cpp
// OLD:
rwhi->InsertVisualStateCallback(base::BindOnce([](shared_ptr<SnapState> s, ..., bool ready) {
    view->CopyFromSurface(..., callback);
}, st, ctrl));

// NEW:
rwhi->ForceRedrawWithCallback(base::BindOnce([](shared_ptr<SnapState> s, ...) {
    if (s->done) return;
    // Wait 167ms for CoreAnimation to composite
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::GrabViewSnapshotWithFreshnessCheck,
                       ctrl, s, /*retry_count=*/0),
        base::Milliseconds(167));
}, st, ctrl));
```

New helper method `GrabViewSnapshotWithFreshnessCheck`:
```cpp
void AbpController::GrabViewSnapshotWithFreshnessCheck(
    shared_ptr<SnapState> s, int retry_count) {
  if (s->done) return;
  WebContents* wc = FindWebContents(s->tab_id);
  if (!wc) { s->done = true; std::move(s->cb).Run({}); return; }

  auto* view = wc->GetNativeView();
  gfx::Rect bounds = wc->GetRenderWidgetHostView()->GetViewBounds();
  ui::GrabViewSnapshot(view, gfx::Rect(bounds.size()),
      base::BindOnce([](shared_ptr<SnapState> s, weak_ptr<AbpController> ctrl,
                        int retry_count, gfx::Image image) {
          if (s->done) return;
          // Freshness check: blank image = stale
          if (image.IsEmpty() && retry_count < 5) {
              base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
                  FROM_HERE,
                  base::BindOnce(&AbpController::GrabViewSnapshotWithFreshnessCheck,
                                 ctrl, s, retry_count + 1),
                  base::Milliseconds(167));
              return;
          }
          s->done = true;
          // ... encode and return via callback ...
      }, s, ctrl->weak_factory_.GetWeakPtr(), retry_count));
}
```

**Keep the existing 750ms outer timeout** as a hard safety net. If neither ForceRedraw callback nor widget-ready callback fires within 750ms, the timeout fires and returns an empty screenshot.

### Also update non-action screenshot path

Apply the same ForceRedraw + GrabViewSnapshot pattern to `DoCaptureScreenshotWithCursor` (API `/screenshot` endpoint).

---

## Fix 2: Deterministic Debugger.pause via Runtime.evaluate Trigger

### Problem

```
Current PauseExecution:
  1. Emulation.setVirtualTimePolicy("pause")  → response ✓
  2. Debugger.pause                           → response ✓ (flag set, NOT actually paused)
  3. Set phase = kPaused                      → WRONG: debugger hasn't actually paused
  4. Run callback

If no JS runs after step 2, the debugger NEVER enters paused state.
The pending pause flag lingers until next ResumeExecution.
```

### Solution

After `Debugger.pause`, send `Runtime.evaluate("void 0")` (without `disableBreaks`) to force V8 to hit the pending pause flag. Wait for the `Debugger.paused` **CDP event** (not command response) to confirm.

```
Fixed PauseExecution:
  1. Emulation.setVirtualTimePolicy("pause")  → wait for response
  2. Debugger.pause                           → wait for response (flag set)
  3. Runtime.evaluate("void 0")               → FIRE-AND-FORGET
       V8 executes → hits pending pause flag → enters paused state
       Debugger.paused EVENT fires
       Runtime.evaluate response HANGS (blocked — expected, ignored)
  4. Wait for "Debugger.paused" CDP event     → DETERMINISTIC
  5. Set phase = kPaused
  6. Run callback
```

### V8 mechanics (verified in source)

```
v8-debugger-agent-impl.cc:
  Debugger.pause → m_pauseOnNextCallRequested = true
                 → v8::debug::SetBreakOnNextFunctionCall(isolate)

v8-runtime-agent-impl.cc:
  Runtime.evaluate("void 0", disableBreaks: false)
    → mode = kDefault (breaks allowed)
    → v8::debug::EvaluateGlobal(isolate, source, mode)
    → V8 enters execution → checks break_on_next_function_call()
    → flag is set → Debug::Break() → fires Debugger.paused event
    → V8 BLOCKS inside EvaluateGlobal (waiting for resume)

  Runtime.evaluate("void 0", disableBreaks: true)
    → mode = kDisableBreaks
    → DisableBreak scope created → break_disabled() returns true
    → Debug::Break() returns immediately → no pause triggered
    → Response returned normally
```

### Callback chain diagram

```
PauseExecution(tab_id, then)
  │
  ├── phase = kPausing
  │
  ├── CDP: Emulation.setVirtualTimePolicy("pause")
  │   └── callback: OnVirtualTimePaused
  │
  │   OnVirtualTimePaused:
  │   ├── CDP: Debugger.pause
  │   │   └── callback: OnDebuggerPauseCommandSent        ← RENAMED (was OnDebuggerPaused)
  │   │
  │   │   OnDebuggerPauseCommandSent:
  │   │   ├── Store tab_state.pause_completion_callback = std::move(then)
  │   │   ├── Start 2s timeout timer (safety net)
  │   │   └── CDP: Runtime.evaluate("void 0")              ← FIRE-AND-FORGET (triggers pause)
  │   │       (callback: no-op lambda, response will hang until resume)
  │   │
  │   │   [CDP event "Debugger.paused" arrives via event listener]
  │   │   └── OnDebuggerPausedEvent(tab_id):
  │   │       ├── Cancel 2s timeout
  │   │       ├── phase = kPaused
  │   │       └── std::move(pause_completion_callback).Run()
  │   │
  │   │   [2s timeout fires (event never arrived)]
  │   │   └── OnPauseConfirmationTimeout(tab_id):
  │   │       ├── LOG(WARNING) "Debugger.paused event not received"
  │   │       ├── phase = kPaused (assume paused)
  │   │       └── std::move(pause_completion_callback).Run()
```

### Changes

**`chrome/browser/abp/abp_controller.h`** — Add to TabState:
```cpp
struct TabState {
  // ... existing fields ...

  // Pending callback for deterministic pause confirmation.
  // Set during PauseExecution, fired when Debugger.paused event arrives.
  base::OnceClosure pause_completion_callback;

  // Timer for pause confirmation timeout (safety net).
  base::OneShotTimer pause_confirmation_timer;
};
```

Add method declaration:
```cpp
void OnDebuggerPausedEvent(const std::string& tab_id);
void OnPauseConfirmationTimeout(const std::string& tab_id);
```

**`chrome/browser/abp/abp_controller.cc`**:

1. **Rename** `OnDebuggerPaused` → `OnDebuggerPauseCommandSent` (handles command *response*, not event)

2. **In `OnDebuggerPauseCommandSent`**: Don't set phase to kPaused. Instead, store callback and send trigger:
```cpp
void AbpController::OnDebuggerPauseCommandSent(
    const std::string& tab_id, base::OnceClosure then,
    bool success, const std::string& result) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) { std::move(then).Run(); return; }

  // Store callback — will be fired by OnDebuggerPausedEvent
  it->second.pause_completion_callback = std::move(then);

  // Start 2s safety timeout
  it->second.pause_confirmation_timer.Start(
      FROM_HERE, base::Seconds(2),
      base::BindOnce(&AbpController::OnPauseConfirmationTimeout,
                     weak_factory_.GetWeakPtr(), tab_id));

  // Send trigger evaluate to force V8 to hit the pending pause
  AbpCdpClient* client = GetCdpClient(tab_id);
  if (client) {
    base::Value::Dict params;
    params.Set("expression", "void 0");
    // NOTE: disableBreaks NOT set — we WANT this to trigger the pause
    client->SendCommand("Runtime.evaluate", params,
        base::BindOnce([](bool, const std::string&) {}));  // response will hang
  }
}
```

3. **Add Debugger.paused event handling** in the CDP event listener lambda (line ~2814):
```cpp
// Handle Debugger.paused event for deterministic pause confirmation
if (method == "Debugger.paused") {
  controller->OnDebuggerPausedEvent(tab);
}
```

4. **Implement event handlers**:
```cpp
void AbpController::OnDebuggerPausedEvent(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  if (!it->second.pause_completion_callback) return;

  it->second.pause_confirmation_timer.Stop();
  it->second.execution.phase = ExecutionPhase::kPaused;
  std::move(it->second.pause_completion_callback).Run();
}

void AbpController::OnPauseConfirmationTimeout(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  if (!it->second.pause_completion_callback) return;

  LOG(WARNING) << "ABP: Debugger.paused event not received within 2s for tab "
               << tab_id << ", proceeding anyway";
  it->second.execution.phase = ExecutionPhase::kPaused;
  std::move(it->second.pause_completion_callback).Run();
}
```

### Add `disableBreaks: true` to utility Runtime.evaluate calls

| Call site | Purpose | disableBreaks |
|-----------|---------|:---:|
| CSS markup injection | Inject `<style>` for outlines | `true` |
| CSS markup cleanup | Remove `<style>` element | `true` |
| GetScrollPosition | Read `window.scrollX/Y` | `true` |
| Scroll action | `window.scrollBy(dx, dy)` | `true` |
| Custom wait polling | Text/URL condition checks | `true` |
| User Execute action | User-provided script | **omit** |
| Pause trigger | `"void 0"` to force pause | **omit** |

---

## Full Action Lifecycle with All Fixes

```
Action Start
  │
  ├── ResumeExecutionIfNeeded
  │   ├── Debugger.resume → callback
  │   │   (if fails: Debugger.disable → Debugger.enable to clear pending pause)
  │   ├── Emulation.setVirtualTimePolicy("realtime") → callback
  │   ├── Page.bringToFront (fire-and-forget, kick compositor)
  │   └── phase = kRunning
  │
  ├── CaptureBeforeScreenshot
  │   ├── Runtime.evaluate: inject CSS (disableBreaks: true)           ← FIX 2
  │   ├── ForceRedrawWithCallback()                                    ← FIX 1
  │   │   ├── blink_widget_ bound? → ForceRedraw on associated pipe
  │   │   └── blink_widget_ null?  → queued, fires on BindWidgetInterfaces
  │   ├── Wait 167ms (CoreAnimation delay)
  │   ├── GrabViewSnapshot (OS surface)                                ← FIX 1
  │   │   └── blank? → retry after 167ms (max 5)
  │   ├── Runtime.evaluate: remove CSS (disableBreaks: true)           ← FIX 2
  │   └── Encode to base64 (ThreadPool)
  │
  ├── ExecuteAction (click/type/scroll/navigate)
  │   └── OnActionDispatched
  │
  ├── DoWaitUntil (load + DOMContentLoaded + paint + networkidle + min_wait)
  │
  ├── StopEventCaptureAndGetScrollPosition
  │   ├── Stop event capture → captured_events_
  │   ├── [if externally paused: resume first]
  │   └── Runtime.evaluate: scrollX/Y (disableBreaks: true)           ← FIX 2
  │
  ├── EnsureVirtualCursorVisible
  │   ├── SetVirtualCursorEnabledViaMojo (associated pipe)
  │   └── SetVirtualCursorViaMojo (associated pipe)
  │
  ├── CaptureAfterScreenshot
  │   ├── Runtime.evaluate: inject CSS (disableBreaks: true)           ← FIX 2
  │   ├── ForceRedrawWithCallback()                                    ← FIX 1
  │   │   (associated pipe — ORDERED after cursor position set above)
  │   ├── Wait 167ms → GrabViewSnapshot (blank? retry)                ← FIX 1
  │   ├── Runtime.evaluate: remove CSS (disableBreaks: true)           ← FIX 2
  │   └── Encode to base64 (ThreadPool)
  │
  ├── PauseExecutionIfNeeded
  │   ├── Emulation.setVirtualTimePolicy("pause") → callback
  │   ├── Debugger.pause → command response
  │   ├── Runtime.evaluate("void 0") → fire-and-forget (trigger)      ← FIX 2
  │   ├── Wait for Debugger.paused CDP event (2s timeout)              ← FIX 2
  │   └── phase = kPaused
  │
  ├── FinalizeResponse → RecordHistory → Build JSON envelope
  │
  └── SendResponse → ReleaseDeterministicSlot → prevent_destroy_ = nullptr
```

---

## ForceRedraw Race Condition Analysis

### Race 1: blink_widget_ null during cross-process navigation — FIXED
- **Before**: Blind 750ms timeout, hope widget reconnects
- **After**: `ForceRedrawWithCallback` queues callback in `pending_on_widget_bound_callbacks_`. When `BindWidgetInterfaces` fires (new renderer connects), callback runs and ForceRedraw retries immediately. Zero wasted latency. 750ms outer timeout remains as hard safety net.

### Race 2: Frame committed between ForceRedraw and GrabViewSnapshot
- **Trigger**: Compositor produces another frame between presentation callback + 167ms delay and snapshot
- **Impact**: GrabViewSnapshot captures a newer frame. This is fine — newer frame includes all our content plus any additional changes.
- **Severity**: Harmless. No fix needed.

### Race 3: ForceRedraw callback orphaned (pipe reset during navigation)
- **Trigger**: `BindWidgetInterfaces` resets `blink_widget_`, destroying the associated pipe while ForceRedraw Mojo message is in flight
- **Impact**: Original ForceRedraw callback orphaned. But `BindWidgetInterfaces` fires `pending_on_widget_bound_callbacks_`, which re-queues ForceRedraw.
- **Severity**: Handled by the widget-ready mechanism. No additional fix needed.

### Race 4: GrabViewSnapshot returns stale/blank image
- **Trigger**: CoreAnimation hasn't composited the new frame to the window server buffer yet
- **Impact**: Blank or stale screenshot captured
- **Mitigation**: Freshness check + retry loop (up to 5 retries, 167ms apart). Total budget: 835ms.

---

## Files to Modify

| File | Change |
|------|--------|
| `content/browser/renderer_host/render_widget_host_impl.h` | Add `ForceRedrawWithCallback()`, `pending_on_widget_bound_callbacks_` |
| `content/browser/renderer_host/render_widget_host_impl.cc` | Implement `ForceRedrawWithCallback()`, fire pending callbacks in `BindWidgetInterfaces` |
| `chrome/browser/abp/abp_controller.h` | Add `pause_completion_callback`, `pause_confirmation_timer` to TabState; add `OnDebuggerPausedEvent()`, `OnPauseConfirmationTimeout()`, `GrabViewSnapshotWithFreshnessCheck()` |
| `chrome/browser/abp/abp_controller.cc` | Replace InsertVisualStateCallback+CopyFromSurface with ForceRedraw+GrabViewSnapshot; modify PauseExecution for deterministic pause; add `disableBreaks` to utility evaluates; add `Debugger.paused` event handling |

---

## Verification

1. **Build**: `autoninja -C out/Default chrome`
2. **Launch**: `./out/Default/Chromium.app/Contents/MacOS/Chromium --enable-abp --abp-session-dir=sessions/test --no-first-run`
3. **Test cursor in screenshot**: Navigate to a page, click at (100, 200), verify cursor appears at (100, 200) in the after-screenshot
4. **Test deterministic pause**: Enable execution control, run a click action, verify LOG contains "Debugger.paused event" before action response is sent
5. **Test CSS markup**: Request screenshot with `markup: "interactive"`, verify outlines appear on buttons/inputs
6. **Test cross-origin navigation**: Navigate from example.com to google.com, verify screenshot doesn't hang (widget-ready callback fires)
7. **Test blank retry**: With display asleep assertion disabled, verify retry loop handles blank screenshots gracefully
