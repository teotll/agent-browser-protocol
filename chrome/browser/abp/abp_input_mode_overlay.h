// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_INPUT_MODE_OVERLAY_H_
#define CHROME_BROWSER_ABP_ABP_INPUT_MODE_OVERLAY_H_

#include "base/scoped_observation.h"
#include "chrome/browser/abp/abp_controller.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"

// Draws a yellow gradient fade border around the web content viewport
// when the browser is in human input mode.
class AbpInputModeOverlay : public views::View,
                            public views::ViewObserver,
                            public abp::AbpController::InputModeObserver {
  METADATA_HEADER(AbpInputModeOverlay, views::View)

 public:
  explicit AbpInputModeOverlay(views::View* contents_view);
  AbpInputModeOverlay(const AbpInputModeOverlay&) = delete;
  AbpInputModeOverlay& operator=(const AbpInputModeOverlay&) = delete;
  ~AbpInputModeOverlay() override;

  // views::View:
  void OnPaint(gfx::Canvas* canvas) override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // AbpController::InputModeObserver:
  void OnInputModeChanged(abp::AbpController::InputMode mode) override;

 private:
  static constexpr int kBorderThickness = 10;

  base::ScopedObservation<views::View, views::ViewObserver>
      bounds_observer_{this};
};

#endif  // CHROME_BROWSER_ABP_ABP_INPUT_MODE_OVERLAY_H_
