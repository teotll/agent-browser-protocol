// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/frame/virtual_cursor_overlay_delegate.h"

#include "base/logging.h"
#include "third_party/blink/renderer/core/inspector/inspector_cursor_drawer.h"
#include "third_party/blink/renderer/platform/graphics/graphics_context.h"
#include "third_party/blink/renderer/platform/graphics/paint/drawing_recorder.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"

namespace blink {

VirtualCursorOverlayDelegate::VirtualCursorOverlayDelegate() {
  LOG(INFO) << "ABP DEBUG L4: VirtualCursorOverlayDelegate constructed";
}

VirtualCursorOverlayDelegate::~VirtualCursorOverlayDelegate() {
  LOG(INFO) << "ABP DEBUG L4: VirtualCursorOverlayDelegate destroyed";
}

void VirtualCursorOverlayDelegate::SetPosition(float x, float y) {
  LOG(INFO) << "ABP DEBUG L4: SetPosition"
            << " old=(" << x_ << ", " << y_ << ")"
            << " new=(" << x << ", " << y << ")";
  x_ = x;
  y_ = y;
}

void VirtualCursorOverlayDelegate::SetVisible(bool visible) {
  LOG(INFO) << "ABP DEBUG L4: SetVisible"
            << " old=" << visible_ << " new=" << visible;
  visible_ = visible;
}

void VirtualCursorOverlayDelegate::SetCursorType(
    ui::mojom::blink::CursorType cursor_type) {
  LOG(INFO) << "ABP DEBUG L4: SetCursorType"
            << " old=" << static_cast<int>(cursor_type_)
            << " new=" << static_cast<int>(cursor_type);
  cursor_type_ = cursor_type;
}

void VirtualCursorOverlayDelegate::SetDeviceScaleFactor(float scale) {
  LOG(INFO) << "ABP DEBUG L4: SetDeviceScaleFactor"
            << " old=" << device_scale_factor_ << " new=" << scale;
  device_scale_factor_ = scale;
}

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

  // Use cached drawing if nothing changed.
  if (DrawingRecorder::UseCachedDrawingIfPossible(
          graphics_context, frame_overlay, DisplayItem::kFrameOverlay)) {
    LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - using cached drawing";
    return;
  }

  // Scale position from CSS pixels to device pixels.
  float scaled_x = x_ * device_scale_factor_;
  float scaled_y = y_ * device_scale_factor_;

  LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - drawing cursor at ("
            << scaled_x << ", " << scaled_y << ")"
            << " scale=" << device_scale_factor_;

  // Record the drawing.
  DrawingRecorder recorder(graphics_context, frame_overlay,
                           DisplayItem::kFrameOverlay, gfx::Rect(view_size));

  // Draw the cursor using the shared cursor drawer.
  // Scale both position and cursor size by device scale factor.
  cc::PaintCanvas* canvas = graphics_context.Canvas();
  if (canvas) {
    InspectorCursorDrawer::DrawCursor(canvas, cursor_type_,
                                      gfx::PointF(scaled_x, scaled_y),
                                      device_scale_factor_);
    LOG(INFO) << "ABP DEBUG L4: PaintFrameOverlay - DrawCursor completed";
  } else {
    LOG(WARNING) << "ABP DEBUG L4: PaintFrameOverlay - CANVAS IS NULL!";
  }
}

}  // namespace blink
