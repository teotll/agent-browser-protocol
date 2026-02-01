# Virtual Cursor Mojo Rendering Design

**Date:** 2026-01-31
**Status:** Approved
**Author:** Claude + Han Wang

## Problem Statement

The virtual cursor doesn't render until the mouse is first moved because the Mojo rendering path has unimplemented TODO stubs in `WebFrameWidgetImpl`. The CDP `Overlay.setVirtualCursor` path works but has startup timing issues.

## Goals

1. **Always-visible cursor**: Virtual cursor renders immediately when ABP is enabled
2. **Native rendering**: Use Mojo/compositor path (no CDP for rendering)
3. **Cursor style feedback**: Cursor shape changes based on element under it (pointer → hand over links, I-beam over text, etc.)
4. **Page interaction preserved**: Hover states and JS events continue working via CDP `Input.dispatchMouseEvent`
5. **Per-tab cursor state**: Each tab maintains its own cursor position
6. **New tabs start centered**: Virtual cursor always starts centered for new tabs

## Architecture

### Current Flow (Broken)

```
Browser Process                         Renderer Process
─────────────────                       ─────────────────
AbpController                           WebFrameWidgetImpl
  │                                       │
  ├─► SetVirtualCursorEnabledViaMojo ───► SetEnabled()
  │                                         └─► TODO: does nothing
  ├─► SetVirtualCursorViaMojo ──────────► SetPosition()
  │                                         └─► TODO: does nothing
  │
  └─► CDP Overlay.setVirtualCursor ─────► VirtualCursorTool (renders)
```

### New Flow

```
Browser Process                         Renderer Process (Blink)
─────────────────                       ────────────────────────
AbpController                           WebFrameWidgetImpl
  │                                       │
  ├─► SetVirtualCursorEnabledViaMojo ───► SetEnabled()
  │                                         └─► VirtualCursorLayerManager::SetEnabled()
  │                                               └─► Creates/destroys cursor layer
  │
  └─► SetVirtualCursorViaMojo ──────────► SetPosition()
                                            ├─► VirtualCursorLayerManager::SetPosition()
                                            │     └─► Updates layer transform
                                            └─► DetectCursorStyleAtPosition()
                                                  ├─► EventHandler::CursorForHitTest()
                                                  └─► VirtualCursorLayerManager::SetCursorType()
```

## Component Changes

### 1. WebFrameWidgetImpl

**Current State (Stubs):**
```cpp
void WebFrameWidgetImpl::SetEnabled(bool enabled) {
  virtual_cursor_enabled_ = enabled;
  // TODO(ABP): Create or destroy the cursor layer
}

void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;
  // TODO(ABP): Update the compositor layer position
}
```

**New Implementation:**
```cpp
// Add member
std::unique_ptr<content::VirtualCursorLayerManager> virtual_cursor_manager_;

void WebFrameWidgetImpl::SetEnabled(bool enabled) {
  virtual_cursor_enabled_ = enabled;
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetEnabled(enabled);
  }
}

void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;

  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetPosition(x, y, visible);

    // Hit-test and update cursor type
    ui::mojom::CursorType cursor_type = DetectCursorStyleAtPosition(x, y);
    virtual_cursor_manager_->SetCursorType(cursor_type);
  }
}
```

### 2. Lifecycle Hooks

**InitializeCompositing():** Create the `VirtualCursorLayerManager`

**SetRootLayer():** Connect manager to compositor root layer
```cpp
void WebFrameWidgetImpl::SetRootLayer(scoped_refptr<cc::Layer> layer) {
  // ... existing code ...

  // Hook for virtual cursor
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetRootLayer(layer.get());
  }

  widget_base_->LayerTreeHost()->SetRootLayer(std::move(layer));
}
```

### 3. Cursor Style Detection

```cpp
ui::mojom::CursorType WebFrameWidgetImpl::DetectCursorStyleAtPosition(
    float x, float y) {
  LocalFrame* frame = local_root_->GetFrame();
  if (!frame || !frame->View() || !frame->ContentLayoutObject()) {
    return ui::mojom::CursorType::kPointer;
  }

  // Create hit test request
  HitTestRequest::HitTestRequestType hit_type =
      HitTestRequest::kReadOnly | HitTestRequest::kActive |
      HitTestRequest::kAllowChildFrameContent;
  HitTestRequest request(hit_type);

  // Perform hit-test
  HitTestLocation location(PhysicalOffset::FromPointFRound(gfx::PointF(x, y)));
  HitTestResult result(request, location);
  frame->ContentLayoutObject()->HitTest(location, result);

  // Get cursor type from EventHandler
  std::optional<ui::Cursor> cursor =
      frame->GetEventHandler().CursorForHitTest(location, result);

  if (cursor.has_value()) {
    return cursor->type();
  }
  return ui::mojom::CursorType::kPointer;
}
```

### 4. Tab Lifecycle

When a new tab is created, call `CenterCursorInTab` after the tab is ready:
```cpp
void AbpController::OnNewTabReady(const std::string& tab_id) {
  CenterCursorInTab(tab_id, base::DoNothing());
}
```

## Code Removal

### CDP Overlay paths to remove:

**1. `chrome/browser/abp/abp_controller.cc` - `CenterCursorInTab`**

Remove CDP Overlay.enable and Overlay.setVirtualCursor calls (~lines 492-541).

Keep:
```cpp
UpdateVirtualCursorState(tab_id, center_x, center_y);
SetVirtualCursorEnabledViaMojo(wc, true);
SetVirtualCursorViaMojo(wc, center_x, center_y, true);
std::move(callback).Run();
```

**2. `chrome/browser/abp/abp_input_dispatcher.cc` - `Move`**

Remove CDP Overlay.setVirtualCursor calls (~lines 237-290).

Keep Mojo calls and CDP `Input.dispatchMouseEvent` (for page interaction).

## Data Flow

### Mouse Move (End-to-End)

```
Agent calls browser_mouse_move(tab_id, x, y)
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ Browser Process                                                  │
├─────────────────────────────────────────────────────────────────┤
│ AbpInputDispatcher::Move()                                       │
│   ├─► UpdateVirtualCursorState(tab_id, x, y)     [internal state]│
│   ├─► SetVirtualCursorEnabledViaMojo(wc, true)   [Mojo]          │
│   ├─► SetVirtualCursorViaMojo(wc, x, y, true)    [Mojo]          │
│   └─► CDP Input.dispatchMouseEvent(mouseMoved)   [CDP - keeps    │
│                                                   page interaction]│
└─────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ Renderer Process (Blink)                                         │
├─────────────────────────────────────────────────────────────────┤
│ WebFrameWidgetImpl                                               │
│   ├─► SetEnabled(true)                                           │
│   │     └─► VirtualCursorLayerManager::SetEnabled(true)          │
│   │           └─► Creates cursor layer, attaches to root         │
│   │                                                              │
│   └─► SetPosition(x, y, true)                                    │
│         ├─► VirtualCursorLayerManager::SetPosition(x, y, true)   │
│         │     └─► Updates layer transform                        │
│         └─► DetectCursorStyleAtPosition(x, y)                    │
│               ├─► HitTest + CursorForHitTest()                   │
│               └─► VirtualCursorLayerManager::SetCursorType()     │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│ Compositor                                                       │
├─────────────────────────────────────────────────────────────────┤
│ VirtualCursorLayer renders cursor at (x, y) with correct shape   │
└─────────────────────────────────────────────────────────────────┘
```

## Error Handling

| Failure Point | Cause | Mitigation |
|---------------|-------|------------|
| Root layer not set | Compositor not ready | Cache state in manager, apply when `SetRootLayer` called |
| Tab closed during operation | Tab destroyed | Weak pointers, null checks before Mojo calls |
| Hit-test fails | No layout object | Return `kPointer` as default cursor type |
| Mojo channel disconnected | Renderer crash | Browser re-creates channel on new renderer |

## Testing

### Unit Tests

**Location:** `chrome/browser/abp/virtual_cursor_unittest.cc`

```cpp
// VirtualCursorLayerManager tests
TEST(AbpVirtualCursorTest, CachesStateBeforeRootLayer)
TEST(AbpVirtualCursorTest, AttachesCursorLayerToRoot)
TEST(AbpVirtualCursorTest, DetachesCursorLayerOnNullRoot)
TEST(AbpVirtualCursorTest, PositionUpdateUsesTransform)
TEST(AbpVirtualCursorTest, CursorTypeChangeTriggersRepaint)
TEST(AbpVirtualCursorTest, VisibilityToggle)
TEST(AbpVirtualCursorTest, EnabledDisabledLifecycle)
TEST(AbpVirtualCursorTest, AppliesCachedStateOnRootLayerSet)

// VirtualCursorLayer tests
TEST(AbpVirtualCursorTest, DefaultCursorTypeIsPointer)
TEST(AbpVirtualCursorTest, RendersDifferentCursorTypes)
```

### Build Integration

Add to `chrome/browser/abp/BUILD.gn`:
```gn
source_set("unit_tests") {
  testonly = true
  sources = [
    "virtual_cursor_unittest.cc",
  ]
  deps = [
    ":abp",
    "//content/renderer:virtual_cursor",
    "//testing/gtest",
  ]
}
```

### Test Execution

```bash
autoninja -C out/Default unit_tests
./out/Default/unit_tests --gtest_filter="AbpVirtualCursor*"
```

### Manual Testing

| Test Case | Steps | Expected Result |
|-----------|-------|-----------------|
| Startup cursor | Launch Chrome with `--enable-abp` | Cursor visible at center immediately |
| Mouse move | Call `browser_mouse_move(tab_id, 100, 200)` | Cursor moves to (100, 200) |
| Cursor over link | Move cursor over `<a>` element | Cursor changes to hand |
| Cursor over text | Move cursor over text input | Cursor changes to I-beam |
| Hover state | Move cursor over hover-styled element | Element shows hover style |
| Navigation | Navigate to new page | Cursor visible on new page |
| Tab switch | Switch between tabs | Each tab shows its own cursor position |
| New tab | Create new tab via MCP | Cursor appears centered |

## Files Modified

1. `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`
2. `third_party/blink/renderer/core/frame/web_frame_widget_impl.h`
3. `chrome/browser/abp/abp_controller.cc`
4. `chrome/browser/abp/abp_input_dispatcher.cc`
5. `chrome/browser/abp/BUILD.gn`
6. `chrome/browser/abp/virtual_cursor_unittest.cc` (new)

## What's Removed

- `Overlay.enable` CDP calls for cursor
- `Overlay.setVirtualCursor` CDP calls
- CDP dependency for cursor rendering

## What's Kept

- `Input.dispatchMouseEvent` CDP calls (page interaction)
- `VirtualCursorLayerManager` / `VirtualCursorLayer` (already implemented)
- Per-tab cursor state in `AbpController::TabState`
