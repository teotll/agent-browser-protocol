# Screenshot Hanging Issue

## Problem

MCP `browser_screenshot` calls stall/hang indefinitely. The REST API (`/api/v1/tabs`) responds fine, but screenshot requests through MCP freeze.

## Current Code Path

```
MCP browser_screenshot
  -> AbpController::CaptureScreenshotWithCursor()
    -> EnsureCompositorActive()          # resumes if paused
    -> DoCaptureScreenshotWithCursor()
      -> RenderWidgetHostImpl::GetSnapshotFromBrowser(from_surface=true)
        -> RequestForceRedraw(snapshot_id)
          -> blink_widget_->ForceRedraw(callback)   # Mojo IPC to renderer
        -> GotResponseToForceRedraw()
          -> CopyFromSurface()                       # grab compositor buffer
      -> 500ms timeout -> DoCaptureScreenshotFallback (GrabViewSnapshot)
```

## Root Cause: Silent Callback Orphaning

`RequestForceRedraw` in `render_widget_host_impl.cc:3501`:

```cpp
void RenderWidgetHostImpl::RequestForceRedraw(int snapshot_id) {
  if (!blink_widget_) {
    return;  // SILENT FAILURE — callback never fires
  }
  blink_widget_->ForceRedraw(
      base::BindOnce(&RenderWidgetHostImpl::GotResponseToForceRedraw,
                     base::Unretained(this), snapshot_id));
}
```

When `blink_widget_` is null, the function returns without calling the callback. The snapshot request sits in `pending_surface_browser_snapshots_` forever. The 500ms timeout in ABP code rescues it, but adds unnecessary latency.

### When `blink_widget_` becomes null

1. **Cross-process navigation / renderer swap** — `BindWidgetInterfaces()` resets `blink_widget_` before rebinding (line 727)
2. **Renderer crash** — `RendererExited()` resets it (line 2383)

### When ForceRedraw Mojo call blocks

Even when `blink_widget_` is valid, the Mojo call goes to the **renderer main thread**. If the main thread is blocked (heavy JS, debugger pause, layout), the response is delayed indefinitely. Then `CopyFromSurface` retries up to 5 times with no per-retry timeout.

## Three Freeze Scenarios

| Scenario | blink_widget_ | ForceRedraw | CopyFromSurface | Result |
|----------|--------------|-------------|-----------------|--------|
| Renderer swap in progress | null | Silent no-op | Never called | Callback orphaned, 500ms timeout saves |
| Renderer main thread blocked | valid | Mojo queued | Never called | Waits until unblocked or 500ms timeout |
| Compositor has no surface | valid | Succeeds | Retries 5x, returns empty | Falls back to GrabViewSnapshot |

## Proposed Fix: `InsertVisualStateCallback` + `CopyFromSurface`

`RenderWidgetHostImpl` already has a **deterministic compositor roundtrip** API:

```cpp
// render_widget_host.h:349-356
// "Roundtrips through the renderer AND compositor pipeline to ensure that any
//  changes to the contents resulting from operations executed prior to this
//  call are visible on screen."
void InsertVisualStateCallback(VisualStateCallback callback);
```

Implementation (`render_widget_host_impl.cc:2133`):

```cpp
void RenderWidgetHostImpl::InsertVisualStateCallback(VisualStateCallback callback) {
  if (!blink_frame_widget_) {
    std::move(callback).Run(false);  // Immediate failure, not silent!
    return;
  }
  if (!widget_compositor_) {
    blink_frame_widget_->BindWidgetCompositor(
        widget_compositor_.BindNewPipeAndPassReceiver(...));
  }
  widget_compositor_->VisualStateRequest(base::BindOnce(
      [](VisualStateCallback callback) { std::move(callback).Run(true); },
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback), false)));
}
```

### Why this is better

1. **No silent failure** — if `blink_frame_widget_` is null, immediately calls `callback(false)`
2. **Mojo disconnect safety** — `WrapCallbackWithDefaultInvokeIfNotRun` fires `callback(false)` if the pipe breaks
3. **Compositor thread, not main thread** — `WidgetCompositor` is bound on the compositor thread, so it doesn't block on JS or debugger pauses
4. **Deterministic** — callback fires with `true` only after the frame is actually presented to the display compositor

### Pre-check: `renderer_initialized()`

```cpp
bool renderer_initialized() const { return renderer_widget_created_; }
```

If this returns false, skip straight to `GrabViewSnapshot` fallback — don't wait 500ms.

### New code path

```
DoCaptureScreenshotWithCursor()
  1. Check rwhi->renderer_initialized()
     - false -> immediate GrabViewSnapshot fallback
  2. rwhi->InsertVisualStateCallback()
     - callback(false) -> immediate GrabViewSnapshot fallback
     - callback(true)  -> frame is on screen, proceed to:
  3. view->CopyFromSurface()
     - success -> encode and return
     - failure -> GrabViewSnapshot fallback
  4. Retain 500ms safety timeout (in case Mojo pipe hangs without disconnect)
```
