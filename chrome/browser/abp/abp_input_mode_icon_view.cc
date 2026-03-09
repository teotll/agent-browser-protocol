// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_input_mode_icon_view.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/accessibility/view_accessibility.h"

AbpInputModeIconView::AbpInputModeIconView(
    IconLabelBubbleView::Delegate* icon_label_bubble_delegate,
    PageActionIconView::Delegate* page_action_icon_delegate)
    : PageActionIconView(/*command_updater=*/nullptr,
                         /*command_id=*/0,
                         icon_label_bubble_delegate,
                         page_action_icon_delegate,
                         "AbpInputMode") {
  SetVisible(true);  // Always visible when ABP is active
  GetViewAccessibility().SetName(u"Toggle input mode");

  // Register as observer for input mode changes.
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (controller) {
    controller->AddInputModeObserver(this);
    current_mode_ = controller->GetInputMode();
  }
}

AbpInputModeIconView::~AbpInputModeIconView() {
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (controller) {
    controller->RemoveInputModeObserver(this);
  }
}

void AbpInputModeIconView::UpdateImpl() {
  // Always visible when ABP is running.
  SetVisible(abp::AbpController::GetInstance() != nullptr);
}

void AbpInputModeIconView::OnExecuting(ExecuteSource source) {
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (!controller) {
    return;
  }

  // Toggle mode.
  base::Value::Dict params;
  if (current_mode_ == abp::AbpController::InputMode::kAgent) {
    params.Set("input_mode", "human");
  } else {
    params.Set("input_mode", "agent");
  }
  controller->SetInputMode(params, base::DoNothing());
}

views::BubbleDialogDelegate* AbpInputModeIconView::GetBubble() const {
  return nullptr;  // No bubble -- direct toggle.
}

const gfx::VectorIcon& AbpInputModeIconView::GetVectorIcon() const {
  if (current_mode_ == abp::AbpController::InputMode::kHuman) {
    return kAbpHumanIcon;
  }
  return kAbpRobotIcon;
}

void AbpInputModeIconView::OnInputModeChanged(
    abp::AbpController::InputMode mode) {
  current_mode_ = mode;
  UpdateIconImage();
}

BEGIN_METADATA(AbpInputModeIconView)
END_METADATA
