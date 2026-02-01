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
