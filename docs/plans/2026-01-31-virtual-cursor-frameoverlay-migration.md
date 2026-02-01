# Virtual Cursor FrameOverlay Migration Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate virtual cursor rendering from `cc::PictureLayer` to Blink's `FrameOverlay` system to fix compositor paint scheduling issues.

**Architecture:** Replace the current `VirtualCursorLayer` (a `cc::PictureLayer` with `ContentLayerClient`) with a `FrameOverlay::Delegate` that paints via Blink's `GraphicsContext`. The `FrameOverlay` system is already integrated into Blink's document lifecycle, so calling `ScheduleAnimation()` will trigger proper repaints. This is the same mechanism used by the Inspector Overlay for DevTools.

**Tech Stack:** Blink renderer (C++), Mojo IPC, Skia graphics, Chromium compositor

---

## Background

The current implementation uses a `cc::PictureLayer` attached to the compositor's root layer. The layer's `PaintContentsToDisplayList()` method is never called because the compositor doesn't schedule paint commits for it. The Inspector Overlay system (used by DevTools) works because it uses `FrameOverlay`, which is integrated into Blink's document lifecycle and triggers repaints via `ScheduleAnimation()`.

## Files Overview

**Delete:**
- `content/renderer/virtual_cursor_layer.h`
- `content/renderer/virtual_cursor_layer.cc`
- `content/renderer/virtual_cursor_layer_manager.h`
- `content/renderer/virtual_cursor_layer_manager.cc`
- `content/renderer/virtual_cursor_layer_manager_unittest.cc`

**Create:**
- `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h`
- `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.cc`

**Modify:**
- `third_party/blink/renderer/core/frame/web_frame_widget_impl.h`
- `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`
- `third_party/blink/renderer/core/frame/build.gni`
- `content/renderer/BUILD.gn`
- `content/test/BUILD.gn`

---

### Task 1: Create VirtualCursorOverlayDelegate Header

**Files:**
- Create: `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h`

**Step 1: Write the header file**

```cpp
// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_VIRTUAL_CURSOR_OVERLAY_DELEGATE_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_VIRTUAL_CURSOR_OVERLAY_DELEGATE_H_

#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-blink.h"

namespace blink {

// Delegate for painting a virtual cursor overlay.
// Used by WebFrameWidgetImpl to render a software cursor for AI agents.
class CORE_EXPORT VirtualCursorOverlayDelegate final
    : public FrameOverlay::Delegate {
 public:
  VirtualCursorOverlayDelegate();
  ~VirtualCursorOverlayDelegate() override;

  // Set cursor position in viewport coordinates.
  void SetPosition(float x, float y);

  // Set cursor visibility.
  void SetVisible(bool visible);

  // Set cursor type (pointer, hand, text, etc.).
  void SetCursorType(ui::mojom::blink::CursorType cursor_type);

  // FrameOverlay::Delegate implementation.
  void PaintFrameOverlay(const FrameOverlay& frame_overlay,
                         GraphicsContext& graphics_context,
                         const gfx::Size& view_size) const override;

 private:
  float x_ = 0;
  float y_ = 0;
  bool visible_ = false;
  ui::mojom::blink::CursorType cursor_type_ =
      ui::mojom::blink::CursorType::kPointer;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_FRAME_VIRTUAL_CURSOR_OVERLAY_DELEGATE_H_
```

**Step 2: Commit**

```bash
git add third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h
git commit -m "feat(abp): add VirtualCursorOverlayDelegate header

Add header for FrameOverlay::Delegate implementation that will render
the virtual cursor using Blink's overlay system instead of a raw
cc::PictureLayer."
```

---

### Task 2: Create VirtualCursorOverlayDelegate Implementation

**Files:**
- Create: `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.cc`

**Step 1: Write the implementation file**

```cpp
// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h"

#include "third_party/blink/renderer/core/inspector/inspector_cursor_drawer.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
#include "third_party/blink/renderer/platform/graphics/paint/drawing_recorder.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"

namespace blink {

VirtualCursorOverlayDelegate::VirtualCursorOverlayDelegate() = default;

VirtualCursorOverlayDelegate::~VirtualCursorOverlayDelegate() = default;

void VirtualCursorOverlayDelegate::SetPosition(float x, float y) {
  x_ = x;
  y_ = y;
}

void VirtualCursorOverlayDelegate::SetVisible(bool visible) {
  visible_ = visible;
}

void VirtualCursorOverlayDelegate::SetCursorType(
    ui::mojom::blink::CursorType cursor_type) {
  cursor_type_ = cursor_type;
}

void VirtualCursorOverlayDelegate::PaintFrameOverlay(
    const FrameOverlay& frame_overlay,
    GraphicsContext& graphics_context,
    const gfx::Size& view_size) const {
  if (!visible_) {
    return;
  }

  // Use cached drawing if nothing changed.
  if (DrawingRecorder::UseCachedDrawingIfPossible(
          graphics_context, frame_overlay, DisplayItem::kFrameOverlay)) {
    return;
  }

  // Record the drawing.
  DrawingRecorder recorder(graphics_context, frame_overlay,
                           DisplayItem::kFrameOverlay, gfx::Rect(view_size));

  // Draw the cursor using the shared cursor drawer.
  cc::PaintCanvas* canvas = graphics_context.Canvas();
  if (canvas) {
    InspectorCursorDrawer::DrawCursor(canvas, cursor_type_,
                                      gfx::PointF(x_, y_), 1.0f);
  }
}

}  // namespace blink
```

**Step 2: Commit**

```bash
git add third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.cc
git commit -m "feat(abp): implement VirtualCursorOverlayDelegate

Implement FrameOverlay::Delegate that paints the virtual cursor using
InspectorCursorDrawer. The cursor is drawn on the GraphicsContext
during Blink's paint lifecycle."
```

---

### Task 3: Add VirtualCursorOverlayDelegate to build.gni

**Files:**
- Modify: `third_party/blink/renderer/core/frame/build.gni`

**Step 1: Add the new files to the source list**

Find the `blink_core_sources_frame` list and add after `"virtual_keyboard_overlay_changed_observer.h",`:

```gni
  "virtual_cursor_overlay_delegate.cc",
  "virtual_cursor_overlay_delegate.h",
```

**Step 2: Verify build**

Run: `autoninja -C out/Default blink_core`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/frame/build.gni
git commit -m "build: add VirtualCursorOverlayDelegate to blink_core"
```

---

### Task 4: Update WebFrameWidgetImpl Header

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.h`

**Step 1: Remove old includes and add new ones**

Remove this line (around line 60):
```cpp
#include "content/renderer/virtual_cursor_layer_manager.h"
```

Add after `#include "third_party/blink/renderer/core/frame/animation_frame_timing_monitor.h"`:
```cpp
#include "third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h"
```

**Step 2: Update member variables**

Find the virtual cursor section (around lines 1310-1326) and replace:

```cpp
  // Virtual cursor layer manager for compositor-based rendering.
  std::unique_ptr<content::VirtualCursorLayerManager> virtual_cursor_manager_;
```

With:

```cpp
  // Virtual cursor overlay for FrameOverlay-based rendering.
  Member<FrameOverlay> virtual_cursor_overlay_;
  std::unique_ptr<VirtualCursorOverlayDelegate> virtual_cursor_delegate_;
```

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.h
git commit -m "refactor(abp): update WebFrameWidgetImpl for FrameOverlay cursor

Replace VirtualCursorLayerManager with FrameOverlay and
VirtualCursorOverlayDelegate members."
```

---

### Task 5: Update WebFrameWidgetImpl Implementation - Part 1 (Remove Old Code)

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`

**Step 1: Find and remove the SetRootLayer hook**

Find this block in `SetRootLayer()` (around lines 3514-3517):

```cpp
  // Hook for virtual cursor - notify manager of root layer change.
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetRootLayer(layer.get());
  }
```

Remove it entirely.

**Step 2: Find the SetEnabled implementation and update it**

Find the `WebFrameWidgetImpl::SetEnabled(bool enabled)` implementation for virtual cursor. It should contain code like:

```cpp
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetEnabled(enabled);
  }
```

Replace the entire function body with the new implementation (see Task 6).

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "refactor(abp): remove VirtualCursorLayerManager from WebFrameWidgetImpl"
```

---

### Task 6: Update WebFrameWidgetImpl Implementation - Part 2 (Add New Code)

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`

**Step 1: Add include**

Add after the other frame includes:
```cpp
#include "third_party/blink/renderer/core/frame/frame_overlay.h"
```

**Step 2: Update SetEnabled implementation**

Replace the `WebFrameWidgetImpl::SetEnabled(bool enabled)` virtual cursor implementation:

```cpp
void WebFrameWidgetImpl::SetEnabled(bool enabled) {
  if (virtual_cursor_enabled_ == enabled) {
    return;
  }
  virtual_cursor_enabled_ = enabled;

  LocalFrame* frame = LocalRootImpl()->GetFrame();
  if (!frame) {
    return;
  }

  if (enabled) {
    // Create the overlay and delegate.
    virtual_cursor_delegate_ = std::make_unique<VirtualCursorOverlayDelegate>();
    virtual_cursor_overlay_ = MakeGarbageCollected<FrameOverlay>(
        frame, std::move(virtual_cursor_delegate_));

    // Apply any cached state.
    // Note: We need to get a raw pointer since we moved the unique_ptr.
    auto* delegate = static_cast<VirtualCursorOverlayDelegate*>(
        virtual_cursor_overlay_->GetDelegate());
    delegate->SetPosition(virtual_cursor_x_, virtual_cursor_y_);
    delegate->SetVisible(virtual_cursor_visible_);
    delegate->SetCursorType(virtual_cursor_type_);
  } else {
    // Destroy the overlay.
    if (virtual_cursor_overlay_) {
      virtual_cursor_overlay_->Destroy();
      virtual_cursor_overlay_ = nullptr;
    }
    virtual_cursor_delegate_.reset();
  }
}
```

**Step 3: Update SetPosition implementation**

Find `WebFrameWidgetImpl::SetPosition(float x, float y, bool visible)` and update:

```cpp
void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;

  if (virtual_cursor_overlay_) {
    auto* delegate = static_cast<VirtualCursorOverlayDelegate*>(
        virtual_cursor_overlay_->GetDelegate());
    delegate->SetPosition(x, y);
    delegate->SetVisible(visible);

    // Schedule a repaint.
    LocalFrame* frame = LocalRootImpl()->GetFrame();
    if (frame && frame->GetPage()) {
      frame->GetPage()->GetChromeClient().ScheduleAnimation(frame->View());
    }
  }
}
```

**Step 4: Update SetCursorType implementation**

Find `WebFrameWidgetImpl::SetCursorType(ui::mojom::CursorType cursor_type)` and update:

```cpp
void WebFrameWidgetImpl::SetCursorType(ui::mojom::CursorType cursor_type) {
  virtual_cursor_type_ = cursor_type;

  if (virtual_cursor_overlay_) {
    auto* delegate = static_cast<VirtualCursorOverlayDelegate*>(
        virtual_cursor_overlay_->GetDelegate());
    delegate->SetCursorType(cursor_type);

    // Schedule a repaint.
    LocalFrame* frame = LocalRootImpl()->GetFrame();
    if (frame && frame->GetPage()) {
      frame->GetPage()->GetChromeClient().ScheduleAnimation(frame->View());
    }
  }
}
```

**Step 5: Update SetVisible implementation**

Find `WebFrameWidgetImpl::SetVisible(bool visible)` for virtual cursor and update:

```cpp
void WebFrameWidgetImpl::SetVisible(bool visible) {
  virtual_cursor_visible_ = visible;

  if (virtual_cursor_overlay_) {
    auto* delegate = static_cast<VirtualCursorOverlayDelegate*>(
        virtual_cursor_overlay_->GetDelegate());
    delegate->SetVisible(visible);

    // Schedule a repaint.
    LocalFrame* frame = LocalRootImpl()->GetFrame();
    if (frame && frame->GetPage()) {
      frame->GetPage()->GetChromeClient().ScheduleAnimation(frame->View());
    }
  }
}
```

**Step 6: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): implement FrameOverlay-based virtual cursor in WebFrameWidgetImpl

Use FrameOverlay and VirtualCursorOverlayDelegate for cursor rendering.
Call ScheduleAnimation() to trigger repaints when cursor state changes."
```

---

### Task 7: Remove Old Files from content/renderer BUILD.gn

**Files:**
- Modify: `content/renderer/BUILD.gn`

**Step 1: Remove virtual cursor layer files from sources**

Find and remove these lines (around lines 155-158):
```gni
    "virtual_cursor_layer.cc",
    "virtual_cursor_layer.h",
    "virtual_cursor_layer_manager.cc",
    "virtual_cursor_layer_manager.h",
```

**Step 2: Verify build**

Run: `autoninja -C out/Default content/renderer`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add content/renderer/BUILD.gn
git commit -m "build: remove virtual_cursor_layer files from content/renderer"
```

---

### Task 8: Remove Old Unit Test from content/test BUILD.gn

**Files:**
- Modify: `content/test/BUILD.gn`

**Step 1: Remove the unit test file**

Find and remove this line:
```gni
    "//content/renderer/virtual_cursor_layer_manager_unittest.cc",
```

**Step 2: Verify build**

Run: `autoninja -C out/Default content_unittests`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add content/test/BUILD.gn
git commit -m "build: remove virtual_cursor_layer_manager_unittest"
```

---

### Task 9: Delete Old Source Files

**Files:**
- Delete: `content/renderer/virtual_cursor_layer.h`
- Delete: `content/renderer/virtual_cursor_layer.cc`
- Delete: `content/renderer/virtual_cursor_layer_manager.h`
- Delete: `content/renderer/virtual_cursor_layer_manager.cc`
- Delete: `content/renderer/virtual_cursor_layer_manager_unittest.cc`

**Step 1: Delete the files**

```bash
rm content/renderer/virtual_cursor_layer.h
rm content/renderer/virtual_cursor_layer.cc
rm content/renderer/virtual_cursor_layer_manager.h
rm content/renderer/virtual_cursor_layer_manager.cc
rm content/renderer/virtual_cursor_layer_manager_unittest.cc
```

**Step 2: Commit**

```bash
git add -A content/renderer/virtual_cursor_layer*
git commit -m "chore: delete old virtual cursor layer files

These files are replaced by the FrameOverlay-based implementation in
third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.*"
```

---

### Task 10: Full Build and Test

**Step 1: Build chrome**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 2: Run content_unittests to verify no regressions**

Run: `./out/Default/content_unittests --gtest_filter="*"`
Expected: Tests pass (the old virtual cursor tests are removed)

**Step 3: Manual test**

1. Start Chrome with ABP:
```bash
./out/Default/chrome --enable-abp
```

2. In another terminal, test virtual cursor:
```bash
# Get tab ID
TAB_ID=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")

# Enable virtual cursor and set position
curl -X POST "http://localhost:8222/api/v1/tabs/$TAB_ID/execute" \
  -H "Content-Type: application/json" \
  -d '{"script":"true"}'

# Take a screenshot - the cursor should be visible
curl -X POST "http://localhost:8222/api/v1/tabs/$TAB_ID/screenshot" \
  -H "Content-Type: application/json" \
  -d '{"format":"png"}' | python3 -c "import sys,json,base64; open('/tmp/cursor.png','wb').write(base64.b64decode(json.load(sys.stdin)['data']))"
```

3. Open `/tmp/cursor.png` and verify the virtual cursor is visible.

**Step 4: Commit (if any fixes needed)**

If the manual test revealed issues, fix them and commit.

---

### Task 11: Add Unit Test for VirtualCursorOverlayDelegate (Optional)

**Files:**
- Create: `third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate_test.cc`
- Modify: `third_party/blink/renderer/core/frame/build.gni`

**Step 1: Write the test file**

```cpp
// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/frame/frame_overlay.h"
#include "third_party/blink/renderer/core/frame/frame_test_helpers.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_controller.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_record_builder.h"
#include "third_party/blink/renderer/platform/testing/task_environment.h"

namespace blink {
namespace {

class VirtualCursorOverlayDelegateTest : public testing::Test {
 protected:
  VirtualCursorOverlayDelegateTest() {
    helper_.Initialize(nullptr, nullptr, nullptr);
    helper_.GetWebView()->MainFrameViewWidget()->Resize(gfx::Size(800, 600));
    helper_.GetWebView()->MainFrameViewWidget()->UpdateAllLifecyclePhases(
        DocumentUpdateReason::kTest);
  }

  LocalFrame* GetFrame() {
    return helper_.GetWebView()->MainFrameImpl()->GetFrame();
  }

 private:
  test::TaskEnvironment task_environment_;
  frame_test_helpers::WebViewHelper helper_;
};

TEST_F(VirtualCursorOverlayDelegateTest, InitialState) {
  auto delegate = std::make_unique<VirtualCursorOverlayDelegate>();
  // Delegate should be created with cursor not visible.
  // We can't directly check private members, but we can verify
  // it doesn't crash when painting with default state.
  auto* overlay = MakeGarbageCollected<FrameOverlay>(
      GetFrame(), std::move(delegate));
  overlay->UpdatePrePaint();

  PaintRecordBuilder builder;
  overlay->Paint(builder.Context());
  // Should succeed without crashing

  overlay->Destroy();
}

TEST_F(VirtualCursorOverlayDelegateTest, SetPositionAndVisible) {
  auto delegate = std::make_unique<VirtualCursorOverlayDelegate>();
  auto* raw_delegate = delegate.get();

  raw_delegate->SetPosition(100.0f, 200.0f);
  raw_delegate->SetVisible(true);
  raw_delegate->SetCursorType(ui::mojom::blink::CursorType::kHand);

  auto* overlay = MakeGarbageCollected<FrameOverlay>(
      GetFrame(), std::move(delegate));
  overlay->UpdatePrePaint();

  PaintRecordBuilder builder;
  overlay->Paint(builder.Context());
  // Should paint cursor at (100, 200)

  overlay->Destroy();
}

}  // namespace
}  // namespace blink
```

**Step 2: Add to build.gni tests**

In `third_party/blink/renderer/core/frame/build.gni`, add to `blink_core_tests_frame`:
```gni
  "virtual_cursor_overlay_delegate_test.cc",
```

**Step 3: Run the test**

Run: `autoninja -C out/Default blink_unittests && ./out/Default/blink_unittests --gtest_filter="VirtualCursorOverlayDelegateTest*"`
Expected: Tests pass

**Step 4: Commit**

```bash
git add third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate_test.cc
git add third_party/blink/renderer/core/frame/build.gni
git commit -m "test(abp): add VirtualCursorOverlayDelegate unit tests"
```

---

## Summary

This migration:
1. Replaces `cc::PictureLayer` with `FrameOverlay::Delegate`
2. Uses Blink's built-in paint scheduling via `ScheduleAnimation()`
3. Reuses `InspectorCursorDrawer` for cursor rendering
4. Removes ~400 lines of custom compositor layer code
5. Follows the same pattern as Inspector Overlay (proven to work)

After completing all tasks, the virtual cursor should paint correctly because `ScheduleAnimation()` triggers Blink's document lifecycle which includes painting the `FrameOverlay`.
