# Frozen Screenshot Lifecycle

## Problem

The current action lifecycle captures the after-screenshot while the page is still running, then pauses execution in the background. This means:
1. The LLM sees a screenshot of a running page — the frozen state may differ from what was captured
2. Markup overlays are injected, captured, and immediately cleaned up — the LLM never sees markup on the actual frozen screen
3. Between screenshot capture and pause, the page can change

## Design

Reorder the action lifecycle so that:
1. A final frame is drawn with markup overlays
2. JS and virtual time are frozen
3. The after-screenshot is captured from the frozen screen
4. Markup persists on the frozen screen until the next action clears it

### New Action Lifecycle

```
Start()
  → CaptureBeforeScreenshot()        // GrabViewSnapshot from frozen buffer
  → ResumeExecution()                // Debugger.resume + virtual time realtime
  → CleanupPreviousMarkup()          // fire-and-forget Runtime.evaluate
  → ExecuteAction()                  // user-provided action callback
  → WaitForActionComplete()          // min_wait + request tracking + settle
  → EnsureVirtualCursorVisible()
  → InjectMarkupCSS()               // Runtime.evaluate (page still running)
  → ForceRedraw()                    // compositor paints markup frame
  → PauseExecution()                 // Debugger.pause + virtual time pause
  → CoreAnimation delay (50ms)
  → GrabViewSnapshot()              // captures frozen screen with markup
  → FinalizeResponse()              // send HTTP response
  → ReleaseDeterministicSlot()       // markup stays on screen
```

### Changes from Current Flow

| Phase | Current | New |
|-------|---------|-----|
| After-screenshot | Inject markup → ForceRedraw → capture → cleanup markup → send response → pause (background) | Inject markup → ForceRedraw → **pause** → capture from frozen buffer → send response |
| Markup cleanup | Immediately after capture (same action) | **Start of next action** (after resume) |
| Pause timing | Background, after response sent | **Before** screenshot capture |
| Before screenshot | Clean frozen buffer | Frozen buffer with previous action's markup |

### Detailed Sequence

#### After wait completes (OnWaitUntilComplete)

1. **EnsureVirtualCursorVisible** — position virtual cursor
2. **InjectMarkupCSS** — `Runtime.evaluate` with `disableBreaks:true` to inject `<style>` element for markup overlays (page still running, compositor active)
3. **ForceRedraw** — compositor paints a frame with markup overlays + virtual cursor
4. **PauseExecution** — `Debugger.pause` + `Emulation.setVirtualTimePolicy(pause)`. Main thread blocks, virtual time freezes. The last painted frame (with markup) is now the frozen screen buffer.
5. **CoreAnimation delay (50ms)** — wait for macOS window server to composite the frame
6. **GrabViewSnapshot** — capture from frozen OS buffer. Guaranteed to show markup since page is frozen.
7. **FinalizeResponse** — build response envelope and send HTTP response
8. **ReleaseDeterministicSlot** — allow next queued action to start

#### At start of next action (after Start)

1. **CaptureBeforeScreenshot** — `GrabViewSnapshot` from frozen buffer. Shows previous action's markup overlays (which is what the LLM last saw — visual continuity).
2. **ResumeExecution** — `Debugger.resume(disableOnResume)` + `ForceRedrawThenResumeVirtualTime`
3. **CleanupPreviousMarkup** — `Runtime.evaluate` with `disableBreaks:true` to remove the previous `<style>` element. Fire-and-forget, overlaps with action execution.
4. **ExecuteAction** — proceed with the action callback

### Constraints

- **ForceRedraw must precede pause**: ForceRedraw needs `BeginMainFrame` on the main thread. `Debugger.pause` blocks the main thread in a nested RunLoop. So the compositor must paint the markup frame before we pause.
- **Race window**: Between ForceRedraw callback and pause completion, the page is still running (~few ms). But markup CSS is a `<style>` element in the DOM, so any repaint still shows markup. Page content could change minimally, but the window is tiny.
- **Before screenshot shows previous markup**: We cannot clean markup while frozen (no ForceRedraw possible). This is acceptable — it's exactly what the LLM saw in the previous response.

### Tab State: Tracking Previous Markup Tags

Add `last_markup_tags` to the per-tab state so the next action knows which markup to clean up:

```cpp
struct TabState {
  // ... existing fields ...
  std::vector<std::string> last_markup_tags;  // markup left on screen from previous action
};
```

Set after injecting markup in after-screenshot. Read and cleared at start of next action's cleanup.

### Files to Modify

1. **`abp_action_context.cc`** — Reorder `OnWaitUntilComplete` → inject markup → ForceRedraw → pause → snapshot → respond. Add markup cleanup after resume.
2. **`abp_action_context.h`** — Add new flow methods (`InjectMarkupAndCapture`, `OnMarkupInjected`, `OnFinalFrameDrawn`, `CaptureFromFrozenBuffer`).
3. **`abp_controller.cc`** — Add `CleanupMarkupForTab()` method. Modify `CaptureActionScreenshot` or add new capture method that skips markup injection (markup already in DOM). Store `last_markup_tags` in tab state.
4. **`abp_controller.h`** — Add `CleanupMarkupForTab()` declaration. Add `last_markup_tags` to `TabState`.
