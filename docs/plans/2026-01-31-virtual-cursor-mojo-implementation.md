# Virtual Cursor Mojo Rendering Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix virtual cursor to render immediately via Mojo/compositor path, remove CDP Overlay dependency, add cursor style detection via native hit-testing.

**Architecture:** WebFrameWidgetImpl will own a VirtualCursorLayerManager instance. When Mojo calls arrive (SetEnabled, SetPosition), they forward to the manager which controls a compositor layer. Hit-testing via EventHandler::CursorForHitTest detects cursor style on every move.

**Tech Stack:** C++, Chromium Mojo IPC, cc::Layer compositor, Blink hit-testing

---

## Task 1: Add VirtualCursorLayerManager to WebFrameWidgetImpl Header

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.h:1309-1319`

**Step 1: Add forward declaration and include**

At line ~60 (after other includes), add:
```cpp
#include "content/renderer/virtual_cursor_layer_manager.h"
```

**Step 2: Add member variable**

At line ~1318 (after `virtual_cursor_type_`), add:
```cpp
  // Virtual cursor layer manager for compositor-based rendering.
  std::unique_ptr<content::VirtualCursorLayerManager> virtual_cursor_manager_;
```

**Step 3: Add DetectCursorStyleAtPosition declaration**

In the private section (around line 1300), add:
```cpp
  // Detects cursor style at given position via hit-testing.
  ui::mojom::CursorType DetectCursorStyleAtPosition(float x, float y);
```

**Step 4: Verify build compiles**

Run: `autoninja -C out/Default blink_core`
Expected: Build succeeds (may have linker errors until implementation added)

**Step 5: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.h
git commit -m "feat(abp): add VirtualCursorLayerManager member to WebFrameWidgetImpl"
```

---

## Task 2: Implement VirtualCursorLayerManager Creation in InitializeCompositing

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc:2583-2604`

**Step 1: Add include at top of file**

```cpp
#include "content/renderer/virtual_cursor_layer_manager.h"
```

**Step 2: Create manager in InitializeCompositingInternal**

After line 2603 (`GetPage()->DidInitializeCompositing(*AnimationHost());`), add:
```cpp
  // Create virtual cursor layer manager for ABP.
  virtual_cursor_manager_ = std::make_unique<content::VirtualCursorLayerManager>();
```

**Step 3: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): create VirtualCursorLayerManager in InitializeCompositing"
```

---

## Task 3: Hook SetRootLayer to Connect Manager to Compositor

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc:3443-3472`

**Step 1: Add hook before SetRootLayer call**

Find line 3465:
```cpp
    widget_base_->LayerTreeHost()->SetRootLayer(std::move(layer));
```

Change the section starting at line 3461 to:
```cpp
  bool root_layer_exists = !!layer;

  // Hook for virtual cursor - notify manager of root layer change.
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetRootLayer(layer.get());
  }

  if (widget_base_->WillBeDestroyed()) {
    CHECK(!layer);
  } else {
    widget_base_->LayerTreeHost()->SetRootLayer(std::move(layer));
  }
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): hook SetRootLayer to connect VirtualCursorLayerManager"
```

---

## Task 4: Implement SetEnabled to Forward to Manager

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc:867-870`

**Step 1: Update SetEnabled implementation**

Replace lines 867-870:
```cpp
void WebFrameWidgetImpl::SetEnabled(bool enabled) {
  virtual_cursor_enabled_ = enabled;
  // TODO(ABP): Create or destroy the cursor layer
}
```

With:
```cpp
void WebFrameWidgetImpl::SetEnabled(bool enabled) {
  virtual_cursor_enabled_ = enabled;
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetEnabled(enabled);
  }
}
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): implement SetEnabled to forward to VirtualCursorLayerManager"
```

---

## Task 5: Implement DetectCursorStyleAtPosition

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc`

**Step 1: Add required includes at top of file**

```cpp
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_request.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "ui/base/cursor/cursor.h"
```

**Step 2: Add implementation after SetEnabled (around line 875)**

```cpp
ui::mojom::CursorType WebFrameWidgetImpl::DetectCursorStyleAtPosition(
    float x, float y) {
  LocalFrame* frame = local_root_->GetFrame();
  if (!frame || !frame->View() || !frame->ContentLayoutObject()) {
    return ui::mojom::CursorType::kPointer;
  }

  // Create hit test request.
  HitTestRequest::HitTestRequestType hit_type =
      HitTestRequest::kReadOnly | HitTestRequest::kActive |
      HitTestRequest::kAllowChildFrameContent;
  HitTestRequest request(hit_type);

  // Perform hit-test at the cursor position.
  HitTestLocation location(PhysicalOffset::FromPointFRound(gfx::PointF(x, y)));
  HitTestResult result(request, location);

  if (frame->ContentLayoutObject()) {
    frame->ContentLayoutObject()->HitTest(location, result);
  }

  // Get cursor type from EventHandler.
  std::optional<ui::Cursor> cursor =
      frame->GetEventHandler().CursorForHitTest(location, result);

  if (cursor.has_value()) {
    return cursor->type();
  }
  return ui::mojom::CursorType::kPointer;
}
```

**Step 3: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): implement DetectCursorStyleAtPosition via hit-testing"
```

---

## Task 6: Implement SetPosition with Hit-Testing

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc:850-855`

**Step 1: Update SetPosition implementation**

Replace lines 850-855:
```cpp
void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;
  // TODO(ABP): Update the compositor layer position
}
```

With:
```cpp
void WebFrameWidgetImpl::SetPosition(float x, float y, bool visible) {
  virtual_cursor_x_ = x;
  virtual_cursor_y_ = y;
  virtual_cursor_visible_ = visible;

  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetPosition(x, y, visible);

    // Hit-test and update cursor type.
    ui::mojom::CursorType cursor_type = DetectCursorStyleAtPosition(x, y);
    virtual_cursor_manager_->SetCursorType(cursor_type);
  }
}
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): implement SetPosition with hit-testing for cursor style"
```

---

## Task 7: Implement SetCursorType and SetVisible

**Files:**
- Modify: `third_party/blink/renderer/core/frame/web_frame_widget_impl.cc:857-865`

**Step 1: Update SetCursorType implementation**

Replace lines 857-860:
```cpp
void WebFrameWidgetImpl::SetCursorType(ui::mojom::CursorType cursor_type) {
  virtual_cursor_type_ = cursor_type;
  // TODO(ABP): Update the cursor shape in the compositor layer
}
```

With:
```cpp
void WebFrameWidgetImpl::SetCursorType(ui::mojom::CursorType cursor_type) {
  virtual_cursor_type_ = cursor_type;
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetCursorType(cursor_type);
  }
}
```

**Step 2: Update SetVisible implementation**

Replace lines 862-865:
```cpp
void WebFrameWidgetImpl::SetVisible(bool visible) {
  virtual_cursor_visible_ = visible;
  // TODO(ABP): Update cursor layer visibility
}
```

With:
```cpp
void WebFrameWidgetImpl::SetVisible(bool visible) {
  virtual_cursor_visible_ = visible;
  if (virtual_cursor_manager_) {
    virtual_cursor_manager_->SetVisible(visible);
  }
}
```

**Step 3: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add third_party/blink/renderer/core/frame/web_frame_widget_impl.cc
git commit -m "feat(abp): implement SetCursorType and SetVisible forwarding"
```

---

## Task 8: Remove CDP Overlay Code from CenterCursorInTab

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:448-542`

**Step 1: Simplify CenterCursorInTab**

Replace lines 448-542 with:
```cpp
void AbpController::CenterCursorInTab(const std::string& tab_id,
                                       base::OnceClosure callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    LOG(WARNING) << "ABP: Tab not found for cursor centering: " << tab_id;
    std::move(callback).Run();
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    LOG(WARNING) << "ABP: No RenderWidgetHostView for cursor centering";
    std::move(callback).Run();
    return;
  }

  // Get viewport size.
  gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
  double center_x = viewport_size.width() / 2.0;
  double center_y = viewport_size.height() / 2.0;

  LOG(INFO) << "ABP: Centering cursor at (" << center_x << ", " << center_y
            << ") in viewport " << viewport_size.width() << "x"
            << viewport_size.height();

  // Update internal virtual cursor state.
  UpdateVirtualCursorState(tab_id, center_x, center_y);

  // Enable and set virtual cursor via Mojo for on-screen rendering.
  SetVirtualCursorEnabledViaMojo(wc, true);
  SetVirtualCursorViaMojo(wc, center_x, center_y, true);

  std::move(callback).Run();
}
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "refactor(abp): remove CDP Overlay code from CenterCursorInTab"
```

---

## Task 9: Remove CDP Overlay Code from Move

**Files:**
- Modify: `chrome/browser/abp/abp_input_dispatcher.cc:198-294`

**Step 1: Simplify Move function**

Replace lines 198-294 with:
```cpp
void AbpInputDispatcher::Move(const std::string& tab_id,
                              const base::Value::Dict& params,
                              ResponseCallback callback) {
  // Validate params early.
  auto x_opt = params.FindDouble("x");
  auto y_opt = params.FindDouble("y");
  if (!x_opt || !y_opt) {
    controller_->SendError(400, "Missing 'x' or 'y' parameter",
                           std::move(callback));
    return;
  }

  double move_x = *x_opt;
  double move_y = *y_opt;

  // Use AbpActionContext for unified action flow.
  AbpActionContext::Run(
      controller_, tab_id, "move", params,
      // Action callback - performs the cursor move.
      base::BindOnce(
          [](double coord_x, double coord_y, AbpActionContext* ctx) {
            // Update virtual cursor state via controller.
            ctx->controller()->UpdateVirtualCursorState(ctx->tab_id(), coord_x,
                                                        coord_y);

            // Enable and set virtual cursor via Mojo for on-screen rendering.
            content::WebContents* wc = ctx->web_contents();
            if (wc) {
              ctx->controller()->SetVirtualCursorEnabledViaMojo(wc, true);
              ctx->controller()->SetVirtualCursorViaMojo(wc, coord_x, coord_y,
                                                          true);
            }

            AbpCdpClient* client = ctx->client();
            if (!client) {
              ctx->OnActionError("CDP_ERROR", "CDP client lost");
              return;
            }

            // Take a scoped_refptr to keep context alive through async calls.
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Send mouseMoved event for page interaction (hover states, etc.).
            base::Value::Dict move_params;
            move_params.Set("type", "mouseMoved");
            move_params.Set("x", coord_x);
            move_params.Set("y", coord_y);

            client->SendCommand(
                "Input.dispatchMouseEvent", std::move(move_params),
                base::BindOnce(
                    [](double final_x, double final_y,
                       scoped_refptr<AbpActionContext> action_ctx,
                       bool success, const std::string& result) {
                      if (!success) {
                        action_ctx->OnActionError("CDP_ERROR", result);
                        return;
                      }

                      // Set result and signal action complete.
                      base::Value::Dict res;
                      res.Set("status", "moved");
                      res.Set("x", final_x);
                      res.Set("y", final_y);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    coord_x, coord_y, ctx_ref));
          },
          move_x, move_y),
      std::move(callback));
}
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_input_dispatcher.cc
git commit -m "refactor(abp): remove CDP Overlay code from Move, keep Input.dispatchMouseEvent"
```

---

## Task 10: Add Cursor Centering for New Tabs

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:1112-1130`

**Step 1: Add cursor centering after tab creation**

After line 1118 (`tab.Set("url", wc->GetVisibleURL().spec());`), add:
```cpp
    // Center the virtual cursor in the new tab.
    CenterCursorInTab(host->GetId(), base::DoNothing());
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): center virtual cursor when creating new tabs"
```

---

## Task 11: Add Unit Test File Structure

**Files:**
- Create: `chrome/browser/abp/virtual_cursor_unittest.cc`
- Modify: `chrome/browser/abp/BUILD.gn`

**Step 1: Create test file**

Create `chrome/browser/abp/virtual_cursor_unittest.cc`:
```cpp
// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/virtual_cursor_layer_manager.h"

#include "cc/layers/layer.h"
#include "content/renderer/virtual_cursor_layer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {

class AbpVirtualCursorTest : public testing::Test {
 protected:
  void SetUp() override {
    manager_ = std::make_unique<VirtualCursorLayerManager>();
  }

  void TearDown() override {
    manager_.reset();
  }

  std::unique_ptr<VirtualCursorLayerManager> manager_;
};

TEST_F(AbpVirtualCursorTest, CachesStateBeforeRootLayer) {
  // Set state before root layer is available.
  manager_->SetEnabled(true);
  manager_->SetPosition(100.0f, 200.0f, true);
  manager_->SetCursorType(ui::mojom::CursorType::kHand);

  // Cursor layer should not exist yet.
  EXPECT_EQ(manager_->GetCursorLayer(), nullptr);

  // Now set root layer.
  auto root = cc::Layer::Create();
  manager_->SetRootLayer(root.get());

  // Cursor layer should now exist.
  EXPECT_NE(manager_->GetCursorLayer(), nullptr);
}

TEST_F(AbpVirtualCursorTest, AttachesCursorLayerToRoot) {
  auto root = cc::Layer::Create();

  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());

  // Root should have one child (the cursor layer).
  EXPECT_EQ(root->children().size(), 1u);
}

TEST_F(AbpVirtualCursorTest, DetachesCursorLayerOnNullRoot) {
  auto root = cc::Layer::Create();

  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());
  EXPECT_EQ(root->children().size(), 1u);

  // Detach by setting null root.
  manager_->SetRootLayer(nullptr);
  EXPECT_EQ(root->children().size(), 0u);
}

TEST_F(AbpVirtualCursorTest, VisibilityToggle) {
  auto root = cc::Layer::Create();

  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());
  manager_->SetPosition(50.0f, 50.0f, true);

  VirtualCursorLayer* cursor = manager_->GetCursorLayer();
  ASSERT_NE(cursor, nullptr);
  EXPECT_TRUE(cursor->IsCursorVisible());

  manager_->SetVisible(false);
  EXPECT_FALSE(cursor->IsCursorVisible());

  manager_->SetVisible(true);
  EXPECT_TRUE(cursor->IsCursorVisible());
}

TEST_F(AbpVirtualCursorTest, EnabledDisabledLifecycle) {
  auto root = cc::Layer::Create();
  manager_->SetRootLayer(root.get());

  // Initially disabled.
  EXPECT_FALSE(manager_->IsEnabled());
  EXPECT_EQ(manager_->GetCursorLayer(), nullptr);

  // Enable.
  manager_->SetEnabled(true);
  EXPECT_TRUE(manager_->IsEnabled());
  EXPECT_NE(manager_->GetCursorLayer(), nullptr);
  EXPECT_EQ(root->children().size(), 1u);

  // Disable.
  manager_->SetEnabled(false);
  EXPECT_FALSE(manager_->IsEnabled());
  EXPECT_EQ(manager_->GetCursorLayer(), nullptr);
  EXPECT_EQ(root->children().size(), 0u);
}

TEST_F(AbpVirtualCursorTest, AppliesCachedStateOnRootLayerSet) {
  // Set state before root layer.
  manager_->SetEnabled(true);
  manager_->SetPosition(150.0f, 250.0f, true);
  manager_->SetCursorType(ui::mojom::CursorType::kIBeam);

  // Set root layer - cached state should be applied.
  auto root = cc::Layer::Create();
  manager_->SetRootLayer(root.get());

  VirtualCursorLayer* cursor = manager_->GetCursorLayer();
  ASSERT_NE(cursor, nullptr);

  EXPECT_EQ(cursor->GetCursorType(), ui::mojom::CursorType::kIBeam);
  EXPECT_TRUE(cursor->IsCursorVisible());
  gfx::PointF pos = cursor->GetCursorPosition();
  EXPECT_FLOAT_EQ(pos.x(), 150.0f);
  EXPECT_FLOAT_EQ(pos.y(), 250.0f);
}

TEST_F(AbpVirtualCursorTest, DefaultCursorTypeIsPointer) {
  auto root = cc::Layer::Create();
  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());

  VirtualCursorLayer* cursor = manager_->GetCursorLayer();
  ASSERT_NE(cursor, nullptr);

  EXPECT_EQ(cursor->GetCursorType(), ui::mojom::CursorType::kPointer);
}

}  // namespace content
```

**Step 2: Update BUILD.gn**

Add to end of `chrome/browser/abp/BUILD.gn`:
```gn

source_set("unit_tests") {
  testonly = true
  sources = [
    "virtual_cursor_unittest.cc",
  ]
  deps = [
    "//cc/layers",
    "//content/renderer:virtual_cursor_layer",
    "//content/renderer:virtual_cursor_layer_manager",
    "//testing/gtest",
    "//ui/base/cursor/mojom:cursor_type",
  ]
}
```

**Step 3: Verify build compiles**

Run: `autoninja -C out/Default chrome/browser/abp:unit_tests`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add chrome/browser/abp/virtual_cursor_unittest.cc chrome/browser/abp/BUILD.gn
git commit -m "test(abp): add virtual cursor unit tests"
```

---

## Task 12: Run Unit Tests and Fix Any Issues

**Step 1: Build the tests**

Run: `autoninja -C out/Default chrome/browser/abp:unit_tests`
Expected: Build succeeds

**Step 2: Run the tests**

Run: `./out/Default/unit_tests --gtest_filter="AbpVirtualCursor*"`
Expected: All tests pass

**Step 3: If tests fail, debug and fix**

Common issues:
- Missing includes
- Incorrect accessor names (check VirtualCursorLayer API)
- Build dependency issues in BUILD.gn

**Step 4: Commit any fixes**

```bash
git add -A
git commit -m "fix(abp): fix unit test issues"
```

---

## Task 13: Full Build and Manual Testing

**Step 1: Full Chrome build**

Run: `autoninja -C out/Default chrome`
Expected: Build succeeds with no errors

**Step 2: Launch Chrome with ABP**

Run: `./out/Default/chrome --enable-abp`
Expected: Browser launches, cursor should be visible at center immediately

**Step 3: Test cursor movement via MCP**

Use the MCP tools to move the cursor:
```
browser_mouse_move(tab_id, 100, 200)
```
Expected: Cursor moves to (100, 200)

**Step 4: Test cursor style changes**

Move cursor over a link element.
Expected: Cursor changes to hand shape

Move cursor over text input.
Expected: Cursor changes to I-beam shape

**Step 5: Test new tab behavior**

Create a new tab via MCP.
Expected: Cursor appears centered in new tab

**Step 6: Test tab switching**

Switch between tabs.
Expected: Each tab shows its own cursor position

**Step 7: Commit if any fixes needed**

```bash
git add -A
git commit -m "fix(abp): manual testing fixes"
```

---

## Task 14: Final Cleanup and Documentation

**Step 1: Run git status**

Run: `git status`
Expected: Working tree clean or only expected changes

**Step 2: Run full test suite**

Run: `./out/Default/unit_tests --gtest_filter="AbpVirtualCursor*"`
Expected: All tests pass

**Step 3: Update design doc status**

In `docs/plans/2026-01-31-virtual-cursor-mojo-rendering.md`, change:
```
**Status:** Approved
```
To:
```
**Status:** Implemented
```

**Step 4: Final commit**

```bash
git add docs/plans/2026-01-31-virtual-cursor-mojo-rendering.md
git commit -m "docs: mark virtual cursor mojo rendering design as implemented"
```

---

## Summary of All Commits

1. `feat(abp): add VirtualCursorLayerManager member to WebFrameWidgetImpl`
2. `feat(abp): create VirtualCursorLayerManager in InitializeCompositing`
3. `feat(abp): hook SetRootLayer to connect VirtualCursorLayerManager`
4. `feat(abp): implement SetEnabled to forward to VirtualCursorLayerManager`
5. `feat(abp): implement DetectCursorStyleAtPosition via hit-testing`
6. `feat(abp): implement SetPosition with hit-testing for cursor style`
7. `feat(abp): implement SetCursorType and SetVisible forwarding`
8. `refactor(abp): remove CDP Overlay code from CenterCursorInTab`
9. `refactor(abp): remove CDP Overlay code from Move, keep Input.dispatchMouseEvent`
10. `feat(abp): center virtual cursor when creating new tabs`
11. `test(abp): add virtual cursor unit tests`
12. `fix(abp): fix unit test issues` (if needed)
13. `fix(abp): manual testing fixes` (if needed)
14. `docs: mark virtual cursor mojo rendering design as implemented`
