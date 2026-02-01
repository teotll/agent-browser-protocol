# Virtual Cursor Log-Based Debugging Plan

**Date:** 2026-02-01
**Bugs:**
1. Virtual cursor not visible when browser is launched in ABP mode
2. Virtual cursor not visible after performing mouse actions

**Approach:** Systematic Phase 1 root cause investigation using diagnostic logging at each component boundary before proposing any fixes.

---

## Architecture Overview

The virtual cursor flows through these component boundaries:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  LAYER 1: ABP Controller (Browser UI Thread)                                │
│  chrome/browser/abp/abp_controller.cc                                       │
│  - IsBrowserReady() → CenterCursorInTab() → SetVirtualCursor*ViaMojo()      │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │ RenderWidgetHost API call
                          ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  LAYER 2: RenderWidgetHostImpl (Browser UI Thread)                          │
│  content/browser/renderer_host/render_widget_host_impl.cc                   │
│  - SetVirtualCursorEnabled/Position/Type()                                  │
│  - Stores pending_virtual_cursor_state_ if blink_frame_widget_ not bound    │
│  - Applies pending state in BindFrameWidgetInterfaces()                     │
│  - Sends via virtual_cursor_remote_ Mojo interface                          │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │ Mojo IPC (VirtualCursor.mojom)
                          ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  LAYER 3: WebFrameWidgetImpl (Renderer Main Thread)                         │
│  third_party/blink/renderer/core/frame/web_frame_widget_impl.cc             │
│  - Receives SetEnabled/SetPosition/SetCursorType/SetVisible                 │
│  - Creates VirtualCursorOverlayDelegate + FrameOverlay on SetEnabled(true)  │
│  - Updates delegate state and calls ScheduleAnimation() for repaint         │
└─────────────────────────┬───────────────────────────────────────────────────┘
                          │ FrameOverlay paint callback
                          ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  LAYER 4: VirtualCursorOverlayDelegate (Renderer Paint Thread)              │
│  third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.cc   │
│  - PaintFrameOverlay() called during frame paint                            │
│  - Draws cursor via InspectorCursorDrawer::DrawCursor() if visible_=true    │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Phase 1: Add Diagnostic Logging

Add `LOG(INFO)` statements at each component boundary to trace data flow. **Do not fix anything yet.**

### Step 1.1: Layer 1 - ABP Controller Logging

**File:** `chrome/browser/abp/abp_controller.cc`

Add logging to these functions:

```cpp
// In IsBrowserReady() - around line 200
bool AbpController::IsBrowserReady() {
  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      WebContents* wc = browser->tab_strip_model()->GetActiveWebContents();
      RenderWidgetHostView* rwhv = wc ? wc->GetRenderWidgetHostView() : nullptr;
      LOG(INFO) << "ABP DEBUG L1: IsBrowserReady check"
                << " wc=" << (wc ? "valid" : "null")
                << " rwhv=" << (rwhv ? "valid" : "null");
      if (rwhv) {
        return true;
      }
    }
  }
  return false;
}

// In CenterCursorInTab() - where cursor is positioned at startup
void AbpController::CenterCursorInTab(const std::string& tab_id,
                                      base::OnceClosure callback) {
  WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab - WebContents null for tab " << tab_id;
    std::move(callback).Run();
    return;
  }

  RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab - RWHV null for tab " << tab_id;
    std::move(callback).Run();
    return;
  }

  gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
  double center_x = viewport_size.width() / 2.0;
  double center_y = viewport_size.height() / 2.0;

  LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab"
            << " tab=" << tab_id
            << " viewport=" << viewport_size.width() << "x" << viewport_size.height()
            << " center=(" << center_x << ", " << center_y << ")";

  // ... existing code to set cursor ...
}

// In SetVirtualCursorViaMojo()
void AbpController::SetVirtualCursorViaMojo(WebContents* wc, float x, float y, bool visible) {
  RenderWidgetHost* rwh = wc->GetRenderWidgetHostView()
                              ? wc->GetRenderWidgetHostView()->GetRenderWidgetHost()
                              : nullptr;
  LOG(INFO) << "ABP DEBUG L1: SetVirtualCursorViaMojo"
            << " x=" << x << " y=" << y << " visible=" << visible
            << " rwh=" << (rwh ? "valid" : "null");

  if (!rwh) return;
  rwh->SetVirtualCursorPosition(x, y, visible);
}

// In SetVirtualCursorEnabledViaMojo()
void AbpController::SetVirtualCursorEnabledViaMojo(WebContents* wc, bool enabled) {
  RenderWidgetHost* rwh = wc->GetRenderWidgetHostView()
                              ? wc->GetRenderWidgetHostView()->GetRenderWidgetHost()
                              : nullptr;
  LOG(INFO) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo"
            << " enabled=" << enabled
            << " rwh=" << (rwh ? "valid" : "null");

  if (!rwh) return;
  rwh->SetVirtualCursorEnabled(enabled);
}
```

### Step 1.2: Layer 2 - RenderWidgetHostImpl Logging

**File:** `content/browser/renderer_host/render_widget_host_impl.cc`

Add logging to track pending state and Mojo binding:

```cpp
// In SetVirtualCursorEnabled()
void RenderWidgetHostImpl::SetVirtualCursorEnabled(bool enabled) {
  LOG(INFO) << "ABP DEBUG L2: SetVirtualCursorEnabled(" << enabled << ")"
            << " blink_frame_widget_bound=" << (blink_frame_widget_.is_bound() ? "true" : "false")
            << " virtual_cursor_remote_bound=" << (virtual_cursor_remote_.is_bound() ? "true" : "false");

  pending_virtual_cursor_state_.has_enabled = true;
  pending_virtual_cursor_state_.enabled = enabled;

  if (!blink_frame_widget_.is_bound()) {
    LOG(INFO) << "ABP DEBUG L2: SetVirtualCursorEnabled - storing as pending (widget not bound)";
    return;
  }

  // Try to bind and send...
  LOG(INFO) << "ABP DEBUG L2: SetVirtualCursorEnabled - sending via Mojo";
  // ... existing code ...
}

// In SetVirtualCursorPosition()
void RenderWidgetHostImpl::SetVirtualCursorPosition(float x, float y, bool visible) {
  LOG(INFO) << "ABP DEBUG L2: SetVirtualCursorPosition"
            << " x=" << x << " y=" << y << " visible=" << visible
            << " blink_frame_widget_bound=" << (blink_frame_widget_.is_bound() ? "true" : "false")
            << " virtual_cursor_remote_bound=" << (virtual_cursor_remote_.is_bound() ? "true" : "false");

  pending_virtual_cursor_state_.has_position = true;
  pending_virtual_cursor_state_.x = x;
  pending_virtual_cursor_state_.y = y;
  pending_virtual_cursor_state_.visible = visible;

  if (!blink_frame_widget_.is_bound()) {
    LOG(INFO) << "ABP DEBUG L2: SetVirtualCursorPosition - storing as pending (widget not bound)";
    return;
  }

  // Try to bind and send...
  LOG(INFO) << "ABP DEBUG L2: SetVirtualCursorPosition - sending via Mojo";
  // ... existing code ...
}

// In BindFrameWidgetInterfaces() - where pending state is applied
void RenderWidgetHostImpl::BindFrameWidgetInterfaces(...) {
  // ... existing binding code ...

  LOG(INFO) << "ABP DEBUG L2: BindFrameWidgetInterfaces called"
            << " pending_has_enabled=" << pending_virtual_cursor_state_.has_enabled
            << " pending_has_position=" << pending_virtual_cursor_state_.has_position
            << " pending_has_type=" << pending_virtual_cursor_state_.has_type;

  // Apply pending virtual cursor state
  if (pending_virtual_cursor_state_.has_enabled ||
      pending_virtual_cursor_state_.has_position ||
      pending_virtual_cursor_state_.has_type) {
    LOG(INFO) << "ABP DEBUG L2: Applying pending virtual cursor state"
              << " enabled=" << pending_virtual_cursor_state_.enabled
              << " position=(" << pending_virtual_cursor_state_.x
              << ", " << pending_virtual_cursor_state_.y << ")"
              << " visible=" << pending_virtual_cursor_state_.visible;

    // ... existing application code ...

    LOG(INFO) << "ABP DEBUG L2: Pending state applied, remote_bound="
              << (virtual_cursor_remote_.is_bound() ? "true" : "false");
  }
}
```

### Step 1.3: Layer 3 - WebFrameWidgetImpl Logging

**File:** `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`

Add logging to Mojo receiver methods:

```cpp
// In BindVirtualCursor()
void WebFrameWidgetImpl::BindVirtualCursor(
    mojo::PendingAssociatedReceiver<mojom::blink::VirtualCursor> receiver) {
  LOG(INFO) << "ABP DEBUG L3: BindVirtualCursor called";
  virtual_cursor_receiver_.Bind(
      std::move(receiver),
      local_root_->GetTaskRunner(TaskType::kInternalDefault));
  LOG(INFO) << "ABP DEBUG L3: BindVirtualCursor - receiver bound="
            << (virtual_cursor_receiver_.is_bound() ? "true" : "false");
}

// In SetEnabled()
void WebFrameWidgetImpl::SetEnabled(bool enabled) {
  LOG(INFO) << "ABP DEBUG L3: SetEnabled(" << enabled << ")"
            << " current_enabled=" << virtual_cursor_enabled_
            << " delegate=" << (virtual_cursor_delegate_ ? "exists" : "null")
            << " overlay=" << (virtual_cursor_overlay_ ? "exists" : "null");

  virtual_cursor_enabled_ = enabled;

  if (enabled) {
    if (!virtual_cursor_delegate_) {
      LOG(INFO) << "ABP DEBUG L3: SetEnabled - creating delegate and overlay";
      virtual_cursor_delegate_ = new VirtualCursorOverlayDelegate();
      virtual_cursor_overlay_ = MakeGarbageCollected<FrameOverlay>(
          LocalRootImpl()->GetFrame(),
          std::unique_ptr<VirtualCursorOverlayDelegate>(virtual_cursor_delegate_));
      LOG(INFO) << "ABP DEBUG L3: SetEnabled - delegate and overlay created";
    }
    // Apply stored state...
    LOG(INFO) << "ABP DEBUG L3: SetEnabled - scheduling animation";
    ScheduleAnimationForVirtualCursor();
  } else {
    LOG(INFO) << "ABP DEBUG L3: SetEnabled(false) - destroying overlay";
    virtual_cursor_overlay_.Clear();
    virtual_cursor_delegate_ = nullptr;
  }
}

// In SetPosition()
void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  LOG(INFO) << "ABP DEBUG L3: SetPosition"
            << " x=" << x << " y=" << y << " visible=" << visible
            << " enabled=" << virtual_cursor_enabled_
            << " delegate=" << (virtual_cursor_delegate_ ? "exists" : "null");

  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;

  if (virtual_cursor_delegate_) {
    virtual_cursor_delegate_->SetPosition(x, y);
    virtual_cursor_delegate_->SetVisible(visible);

    ui::mojom::CursorType cursor_type = DetectCursorStyleAtPosition(x, y);
    LOG(INFO) << "ABP DEBUG L3: SetPosition - detected cursor type="
              << static_cast<int>(cursor_type);
    virtual_cursor_delegate_->SetCursorType(cursor_type);

    LOG(INFO) << "ABP DEBUG L3: SetPosition - scheduling animation";
    ScheduleAnimationForVirtualCursor();
  } else {
    LOG(INFO) << "ABP DEBUG L3: SetPosition - NO DELEGATE, cursor will not render!";
  }
}

// Helper for scheduling animation (add if not exists)
void WebFrameWidgetImpl::ScheduleAnimationForVirtualCursor() {
  LocalFrame* frame = LocalRootImpl()->GetFrame();
  if (frame && frame->GetPage()) {
    LOG(INFO) << "ABP DEBUG L3: ScheduleAnimation - scheduling via ChromeClient";
    frame->GetPage()->GetChromeClient().ScheduleAnimation(frame->View());
  } else {
    LOG(INFO) << "ABP DEBUG L3: ScheduleAnimation - FAILED, frame or page null";
  }
}
```

### Step 1.4: Layer 4 - VirtualCursorOverlayDelegate Logging

**File:** `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.cc`

Add logging to paint method:

```cpp
void VirtualCursorOverlayDelegate::PaintFrameOverlay(
    const FrameOverlay& frame_overlay,
    GraphicsContext& graphics_context,
    const gfx::Size& view_size) const {

  LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay called"
            << " visible=" << visible_
            << " position=(" << x_ << ", " << y_ << ")"
            << " cursor_type=" << static_cast<int>(cursor_type_)
            << " view_size=" << view_size.width() << "x" << view_size.height();

  if (!visible_) {
    LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - NOT VISIBLE, skipping draw";
    return;
  }

  // Check for cached drawing
  if (DrawingRecorder::UseCachedDrawingIfPossible(graphics_context, *this,
                                                   DisplayItem::kFrameOverlay)) {
    LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - using cached drawing";
    return;
  }

  LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - drawing cursor at ("
            << x_ << ", " << y_ << ")";

  DrawingRecorder recorder(graphics_context, *this, DisplayItem::kFrameOverlay,
                           gfx::Rect(view_size));

  cc::PaintCanvas* canvas = graphics_context.Canvas();
  if (canvas) {
    InspectorCursorDrawer::DrawCursor(canvas, cursor_type_,
                                      gfx::PointF(x_, y_), 1.0f);
    LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - DrawCursor completed";
  } else {
    LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - CANVAS IS NULL!";
  }
}

// Add logging to setters
void VirtualCursorOverlayDelegate::SetPosition(float x, float y) {
  LOG(INFO) << "ABP DEBUG L4: SetPosition delegate"
            << " old=(" << x_ << ", " << y_ << ")"
            << " new=(" << x << ", " << y << ")";
  x_ = x;
  y_ = y;
}

void VirtualCursorOverlayDelegate::SetVisible(bool visible) {
  LOG(INFO) << "ABP DEBUG L4: SetVisible delegate"
            << " old=" << visible_ << " new=" << visible;
  visible_ = visible;
}

void VirtualCursorOverlayDelegate::SetCursorType(ui::mojom::CursorType cursor_type) {
  LOG(INFO) << "ABP DEBUG L4: SetCursorType delegate"
            << " old=" << static_cast<int>(cursor_type_)
            << " new=" << static_cast<int>(cursor_type);
  cursor_type_ = cursor_type;
}
```

---

## Phase 2: Run and Capture Logs

### Step 2.1: Build with Debug Logging

```bash
cd /Users/hanwang/src/src
autoninja -C out/Default chrome
```

### Step 2.2: Capture Startup Logs (Bug #1)

Run Chrome in ABP mode and capture all logs:

```bash
./out/Default/Chromium.app/Contents/MacOS/Chromium \
  --enable-abp \
  --enable-logging \
  --v=0 \
  2>&1 | tee /tmp/cursor-startup.log
```

Wait 5 seconds for browser to fully load, then:

```bash
# Filter for ABP DEBUG logs
grep "ABP DEBUG" /tmp/cursor-startup.log | head -100
```

**Expected trace for working startup:**
```
ABP DEBUG L1: IsBrowserReady check wc=valid rwhv=valid
ABP DEBUG L1: CenterCursorInTab tab=XXX viewport=1200x800 center=(600, 400)
ABP DEBUG L1: SetVirtualCursorEnabledViaMojo enabled=true rwh=valid
ABP DEBUG L2: SetVirtualCursorEnabled(true) blink_frame_widget_bound=true ...
ABP DEBUG L2: SetVirtualCursorEnabled - sending via Mojo
ABP DEBUG L3: SetEnabled(true) current_enabled=false delegate=null overlay=null
ABP DEBUG L3: SetEnabled - creating delegate and overlay
ABP DEBUG L3: SetEnabled - scheduling animation
ABP DEBUG L1: SetVirtualCursorViaMojo x=600 y=400 visible=true rwh=valid
ABP DEBUG L2: SetVirtualCursorPosition x=600 y=400 visible=true ...
ABP DEBUG L2: SetVirtualCursorPosition - sending via Mojo
ABP DEBUG L3: SetPosition x=600 y=400 visible=true enabled=true delegate=exists
ABP DEBUG L4: SetPosition delegate old=(0, 0) new=(600, 400)
ABP DEBUG L4: SetVisible delegate old=false new=true
ABP DEBUG L3: SetPosition - scheduling animation
ABP DEBUG L4: PaintFrameOverlay called visible=true position=(600, 400) ...
ABP DEBUG L4: PaintFrameOverlay - drawing cursor at (600, 400)
ABP DEBUG L4: PaintFrameOverlay - DrawCursor completed
```

**Failure patterns to look for:**

| Log Pattern | Indicates |
|-------------|-----------|
| `SetVirtualCursorEnabled - storing as pending` | Widget not bound at startup |
| `BindFrameWidgetInterfaces ... pending_has_enabled=true` followed by no L3 logs | Pending state not applied correctly |
| L3 `SetEnabled` or `SetPosition` with `delegate=null` | Overlay not created |
| L3 `ScheduleAnimation - FAILED` | Frame/Page not ready |
| L4 `PaintFrameOverlay - NOT VISIBLE` | visible_ flag not set |
| L4 `PaintFrameOverlay - CANVAS IS NULL` | Graphics context issue |
| No L4 logs at all | Paint never called, overlay not in paint tree |

### Step 2.3: Capture Mouse Action Logs (Bug #2)

With browser running, execute a click via API:

```bash
# Get tab ID
TAB_ID=$(curl -s http://localhost:8222/api/v1/tabs | jq -r '.tabs[0].id')

# Click at coordinates
curl -X POST "http://localhost:8222/api/v1/tabs/$TAB_ID/click" \
  -H "Content-Type: application/json" \
  -d '{"x": 100, "y": 100}'

# Filter logs for this action
grep "ABP DEBUG" /tmp/cursor-startup.log | tail -50
```

**Expected trace for working click:**
```
ABP DEBUG L1: SetVirtualCursorViaMojo x=100 y=100 visible=true rwh=valid
ABP DEBUG L2: SetVirtualCursorPosition x=100 y=100 visible=true blink_frame_widget_bound=true ...
ABP DEBUG L2: SetVirtualCursorPosition - sending via Mojo
ABP DEBUG L3: SetPosition x=100 y=100 visible=true enabled=true delegate=exists
ABP DEBUG L4: SetPosition delegate old=(600, 400) new=(100, 100)
ABP DEBUG L3: SetPosition - scheduling animation
ABP DEBUG L4: PaintFrameOverlay called visible=true position=(100, 100) ...
ABP DEBUG L4: PaintFrameOverlay - drawing cursor at (100, 100)
```

---

## Phase 3: Analyze Logs and Identify Root Cause

Based on the log output, identify WHERE the data flow breaks:

### Scenario A: Widget Not Bound at Startup

**Symptoms:**
- L2 shows `storing as pending` on initial calls
- L2 `BindFrameWidgetInterfaces` is called AFTER L1 cursor calls
- Pending state may or may not be applied

**Root cause:** `IsBrowserReady()` returns true before `blink_frame_widget_` is bound.

**Hypothesis:** The `RenderWidgetHostView` exists but the Mojo frame widget interface isn't bound yet.

### Scenario B: Pending State Not Applied

**Symptoms:**
- L2 `BindFrameWidgetInterfaces` shows pending state exists
- But no subsequent L3 logs showing `SetEnabled` or `SetPosition` being called

**Root cause:** Code in `BindFrameWidgetInterfaces` doesn't correctly apply pending state.

**Hypothesis:** The pending state application code has a bug or the Mojo binding fails silently.

### Scenario C: Overlay Not Created

**Symptoms:**
- L3 `SetEnabled(true)` is called
- But L3 shows `delegate=null` in subsequent `SetPosition` calls

**Root cause:** FrameOverlay creation fails or delegate is not assigned.

**Hypothesis:** GC collects the overlay, or the overlay is not properly associated with the frame.

### Scenario D: Animation Not Scheduled

**Symptoms:**
- L3 shows delegate exists and position is set
- L3 `ScheduleAnimation - FAILED` appears
- No L4 `PaintFrameOverlay` logs

**Root cause:** Frame or Page is null when scheduling animation.

**Hypothesis:** The LocalFrame or Page is not fully initialized when cursor is set.

### Scenario E: Paint Not Called

**Symptoms:**
- L3 `ScheduleAnimation` succeeds
- No L4 `PaintFrameOverlay` logs at all

**Root cause:** FrameOverlay is not in the paint tree.

**Hypothesis:** The overlay needs to be explicitly added to the paint hierarchy.

### Scenario F: Cursor Not Visible

**Symptoms:**
- L4 `PaintFrameOverlay called visible=false`

**Root cause:** `SetVisible(true)` was never called or was overwritten.

**Hypothesis:** Order of operations issue - visibility set before delegate exists.

---

## Phase 4: Form Hypothesis and Test

**DO NOT FIX MULTIPLE THINGS AT ONCE.**

Based on log analysis, form ONE hypothesis and test it:

### Example Hypothesis Test

**Hypothesis:** The widget is not bound when `IsBrowserReady()` returns true.

**Test:** Add a check in `IsBrowserReady()` to also verify widget binding:

```cpp
// Temporary diagnostic change
bool AbpController::IsBrowserReady() {
  // ... existing checks ...
  if (rwhv) {
    RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
    RenderWidgetHostImpl* rwhi = static_cast<RenderWidgetHostImpl*>(rwh);
    bool widget_bound = rwhi->IsFrameWidgetBound();  // Add this method
    LOG(INFO) << "ABP DEBUG L1: IsBrowserReady - widget_bound=" << widget_bound;
    if (!widget_bound) {
      return false;  // Wait for widget to be bound
    }
    return true;
  }
}
```

**Verify:** Run browser and check if cursor now appears at startup.

---

## Phase 5: Create Failing Test Case

Before implementing the actual fix, create a test that reproduces the bug:

```cpp
// In chrome/browser/abp/abp_controller_unittest.cc (or similar)

TEST_F(AbpControllerTest, VirtualCursorAppearsAtStartup) {
  // 1. Launch browser with ABP enabled
  // 2. Wait for IsBrowserReady() to return true
  // 3. Take screenshot
  // 4. Assert: cursor is visible in screenshot at center position
}

TEST_F(AbpControllerTest, VirtualCursorUpdatesOnMouseAction) {
  // 1. Launch browser with ABP enabled
  // 2. Execute click at (100, 100)
  // 3. Take screenshot
  // 4. Assert: cursor is visible at (100, 100)
}
```

---

## Summary: Debugging Workflow

1. **Add logging** - Instrument all 4 layers with `ABP DEBUG L1/L2/L3/L4` prefixes
2. **Rebuild** - `autoninja -C out/Default chrome`
3. **Capture logs** - Run with `--enable-logging` and redirect to file
4. **Filter logs** - `grep "ABP DEBUG" logfile`
5. **Identify break point** - Find where the trace stops or shows unexpected values
6. **Form hypothesis** - One specific cause
7. **Test minimally** - One change to test hypothesis
8. **Verify or iterate** - If fix works, write test; if not, new hypothesis

---

## Files to Modify for Logging

| Layer | File | Functions |
|-------|------|-----------|
| L1 | `chrome/browser/abp/abp_controller.cc` | `IsBrowserReady`, `CenterCursorInTab`, `SetVirtualCursor*ViaMojo` |
| L2 | `content/browser/renderer_host/render_widget_host_impl.cc` | `SetVirtualCursor*`, `BindFrameWidgetInterfaces` |
| L3 | `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc` | `BindVirtualCursor`, `SetEnabled`, `SetPosition`, `SetCursorType`, `SetVisible` |
| L4 | `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.cc` | `PaintFrameOverlay`, `SetPosition`, `SetVisible`, `SetCursorType` |

---

## Cleanup After Debugging

Once root cause is identified and fixed:

1. Remove all `ABP DEBUG L*` log statements
2. Keep only essential permanent logging (e.g., errors, significant state changes)
3. Commit fix with proper description of root cause
