// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/virtual_cursor_layer_manager.h"

#include "cc/layers/layer.h"
#include "content/renderer/virtual_cursor_layer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"

namespace content {

class VirtualCursorLayerManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    manager_ = std::make_unique<VirtualCursorLayerManager>();
  }

  void TearDown() override { manager_.reset(); }

  std::unique_ptr<VirtualCursorLayerManager> manager_;
};

TEST_F(VirtualCursorLayerManagerTest, CachesStateBeforeRootLayer) {
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

TEST_F(VirtualCursorLayerManagerTest, AttachesCursorLayerToRoot) {
  auto root = cc::Layer::Create();

  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());

  // Root should have one child (the cursor layer).
  EXPECT_EQ(root->children().size(), 1u);
}

TEST_F(VirtualCursorLayerManagerTest, DetachesCursorLayerOnNullRoot) {
  auto root = cc::Layer::Create();

  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());
  EXPECT_EQ(root->children().size(), 1u);

  // Detach by setting null root.
  manager_->SetRootLayer(nullptr);
  EXPECT_EQ(root->children().size(), 0u);
}

TEST_F(VirtualCursorLayerManagerTest, VisibilityToggle) {
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

TEST_F(VirtualCursorLayerManagerTest, EnabledDisabledLifecycle) {
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

TEST_F(VirtualCursorLayerManagerTest, AppliesCachedStateOnRootLayerSet) {
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

TEST_F(VirtualCursorLayerManagerTest, DefaultCursorTypeIsPointer) {
  auto root = cc::Layer::Create();
  manager_->SetEnabled(true);
  manager_->SetRootLayer(root.get());

  VirtualCursorLayer* cursor = manager_->GetCursorLayer();
  ASSERT_NE(cursor, nullptr);

  EXPECT_EQ(cursor->GetCursorType(), ui::mojom::CursorType::kPointer);
}

}  // namespace content
