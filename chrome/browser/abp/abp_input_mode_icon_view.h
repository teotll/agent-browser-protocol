// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_INPUT_MODE_ICON_VIEW_H_
#define CHROME_BROWSER_ABP_ABP_INPUT_MODE_ICON_VIEW_H_

#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/ui/views/page_action/page_action_icon_view.h"
#include "ui/base/metadata/metadata_header_macros.h"

// Address bar icon that displays the current ABP input mode (agent vs human)
// and toggles the mode when clicked.
class AbpInputModeIconView : public PageActionIconView,
                             public abp::AbpController::InputModeObserver {
  METADATA_HEADER(AbpInputModeIconView, PageActionIconView)

 public:
  AbpInputModeIconView(
      IconLabelBubbleView::Delegate* icon_label_bubble_delegate,
      PageActionIconView::Delegate* page_action_icon_delegate);
  AbpInputModeIconView(const AbpInputModeIconView&) = delete;
  AbpInputModeIconView& operator=(const AbpInputModeIconView&) = delete;
  ~AbpInputModeIconView() override;

  // PageActionIconView:
  void UpdateImpl() override;
  void OnExecuting(ExecuteSource source) override;
  views::BubbleDialogDelegate* GetBubble() const override;
  const gfx::VectorIcon& GetVectorIcon() const override;

  // AbpController::InputModeObserver:
  void OnInputModeChanged(abp::AbpController::InputMode mode) override;

 private:
  abp::AbpController::InputMode current_mode_ =
      abp::AbpController::InputMode::kAgent;
};

#endif  // CHROME_BROWSER_ABP_ABP_INPUT_MODE_ICON_VIEW_H_
