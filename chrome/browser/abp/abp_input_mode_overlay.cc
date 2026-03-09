// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_input_mode_overlay.h"

#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_shader.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_f.h"

AbpInputModeOverlay::AbpInputModeOverlay(views::View* contents_view) {
  SetPaintToLayer();
  layer()->SetFillsBoundsOpaquely(false);
  SetCanProcessEventsWithinSubtree(false);
  SetVisible(false);

  bounds_observer_.Observe(contents_view);

  // Register as observer for input mode changes.
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (controller) {
    controller->AddInputModeObserver(this);
    // If already in human mode, show immediately.
    if (controller->GetInputMode() == abp::AbpController::InputMode::kHuman) {
      SetVisible(true);
    }
  }
}

AbpInputModeOverlay::~AbpInputModeOverlay() {
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (controller) {
    controller->RemoveInputModeObserver(this);
  }
}

void AbpInputModeOverlay::OnPaint(gfx::Canvas* canvas) {
  const gfx::Rect bounds = GetLocalBounds();
  if (bounds.IsEmpty()) {
    return;
  }

  // Yellow with ~80% opacity at the edge, fading to fully transparent inward.
  // Original ARGB: (0xCC, 0xF5, 0xC5, 0x42)
  // As SkColor4f {R, G, B, A}: {0.96, 0.77, 0.26, 0.80}
  const SkColor4f edge_color = {0.96f, 0.77f, 0.26f, 0.80f};
  const SkColor4f transparent = {0.0f, 0.0f, 0.0f, 0.0f};

  // Top edge: gradient from top to bottom.
  {
    SkPoint pts[] = {
        {0, SkIntToScalar(bounds.y())},
        {0, SkIntToScalar(bounds.y() + kBorderThickness)},
    };
    SkColor4f colors[] = {edge_color, transparent};
    cc::PaintFlags flags;
    flags.setShader(cc::PaintShader::MakeLinearGradient(
        pts, colors, /*pos=*/nullptr, 2, SkTileMode::kClamp));
    canvas->DrawRect(
        gfx::RectF(bounds.x(), bounds.y(), bounds.width(), kBorderThickness),
        flags);
  }

  // Bottom edge: gradient from bottom to top.
  {
    SkPoint pts[] = {
        {0, SkIntToScalar(bounds.bottom())},
        {0, SkIntToScalar(bounds.bottom() - kBorderThickness)},
    };
    SkColor4f colors[] = {edge_color, transparent};
    cc::PaintFlags flags;
    flags.setShader(cc::PaintShader::MakeLinearGradient(
        pts, colors, /*pos=*/nullptr, 2, SkTileMode::kClamp));
    canvas->DrawRect(
        gfx::RectF(bounds.x(), bounds.bottom() - kBorderThickness,
                    bounds.width(), kBorderThickness),
        flags);
  }

  // Left edge: gradient from left to right.
  {
    SkPoint pts[] = {
        {SkIntToScalar(bounds.x()), 0},
        {SkIntToScalar(bounds.x() + kBorderThickness), 0},
    };
    SkColor4f colors[] = {edge_color, transparent};
    cc::PaintFlags flags;
    flags.setShader(cc::PaintShader::MakeLinearGradient(
        pts, colors, /*pos=*/nullptr, 2, SkTileMode::kClamp));
    canvas->DrawRect(
        gfx::RectF(bounds.x(), bounds.y(), kBorderThickness, bounds.height()),
        flags);
  }

  // Right edge: gradient from right to left.
  {
    SkPoint pts[] = {
        {SkIntToScalar(bounds.right()), 0},
        {SkIntToScalar(bounds.right() - kBorderThickness), 0},
    };
    SkColor4f colors[] = {edge_color, transparent};
    cc::PaintFlags flags;
    flags.setShader(cc::PaintShader::MakeLinearGradient(
        pts, colors, /*pos=*/nullptr, 2, SkTileMode::kClamp));
    canvas->DrawRect(
        gfx::RectF(bounds.right() - kBorderThickness, bounds.y(),
                    kBorderThickness, bounds.height()),
        flags);
  }
}

void AbpInputModeOverlay::OnViewBoundsChanged(views::View* observed_view) {
  SetBoundsRect(observed_view->bounds());
  SchedulePaint();
}

void AbpInputModeOverlay::OnViewIsDeleting(views::View* observed_view) {
  bounds_observer_.Reset();
}

void AbpInputModeOverlay::OnInputModeChanged(
    abp::AbpController::InputMode mode) {
  const bool human_mode =
      (mode == abp::AbpController::InputMode::kHuman);
  SetVisible(human_mode);
  if (human_mode) {
    SchedulePaint();
  }
}

BEGIN_METADATA(AbpInputModeOverlay)
END_METADATA
