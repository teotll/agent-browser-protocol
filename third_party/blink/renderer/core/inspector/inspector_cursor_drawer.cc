// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/inspector/inspector_cursor_drawer.h"

#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/gfx/geometry/point_f.h"

namespace blink {

namespace {

// Create the arrow cursor path (standard pointer)
SkPath CreateArrowPath(float scale) {
  SkPathBuilder builder;
  // Arrow pointing down-right, tip at top-left
  builder.moveTo(1 * scale, 1 * scale);
  builder.lineTo(1 * scale, 17 * scale);
  builder.lineTo(5 * scale, 13 * scale);
  builder.lineTo(8 * scale, 21 * scale);
  builder.lineTo(11 * scale, 20 * scale);
  builder.lineTo(8 * scale, 12 * scale);
  builder.lineTo(13 * scale, 12 * scale);
  builder.close();
  return builder.detach();
}

// Create the hand/pointer cursor path (for links)
SkPath CreateHandPath(float scale) {
  SkPathBuilder builder;
  // Pointing hand shape
  builder.moveTo(8 * scale, 1 * scale);
  builder.lineTo(8 * scale, 9 * scale);
  builder.lineTo(6 * scale, 9 * scale);
  builder.lineTo(6 * scale, 11 * scale);
  builder.lineTo(4 * scale, 11 * scale);
  builder.lineTo(4 * scale, 13 * scale);
  builder.lineTo(2 * scale, 13 * scale);
  builder.lineTo(2 * scale, 18 * scale);
  builder.lineTo(4 * scale, 20 * scale);
  builder.lineTo(14 * scale, 20 * scale);
  builder.lineTo(16 * scale, 18 * scale);
  builder.lineTo(16 * scale, 9 * scale);
  builder.lineTo(14 * scale, 9 * scale);
  builder.lineTo(14 * scale, 7 * scale);
  builder.lineTo(12 * scale, 7 * scale);
  builder.lineTo(12 * scale, 5 * scale);
  builder.lineTo(10 * scale, 5 * scale);
  builder.lineTo(10 * scale, 1 * scale);
  builder.close();
  return builder.detach();
}

// Create the I-beam/text cursor path
SkPath CreateIBeamPath(float scale) {
  SkPathBuilder builder;
  // Top serif
  builder.moveTo(6 * scale, 2 * scale);
  builder.lineTo(10 * scale, 2 * scale);
  builder.lineTo(10 * scale, 4 * scale);
  builder.lineTo(9 * scale, 4 * scale);
  builder.lineTo(9 * scale, 11 * scale);
  builder.lineTo(10 * scale, 11 * scale);
  builder.lineTo(10 * scale, 13 * scale);
  builder.lineTo(9 * scale, 13 * scale);
  builder.lineTo(9 * scale, 20 * scale);
  builder.lineTo(10 * scale, 20 * scale);
  builder.lineTo(10 * scale, 22 * scale);
  builder.lineTo(6 * scale, 22 * scale);
  builder.lineTo(6 * scale, 20 * scale);
  builder.lineTo(7 * scale, 20 * scale);
  builder.lineTo(7 * scale, 13 * scale);
  builder.lineTo(6 * scale, 13 * scale);
  builder.lineTo(6 * scale, 11 * scale);
  builder.lineTo(7 * scale, 11 * scale);
  builder.lineTo(7 * scale, 4 * scale);
  builder.lineTo(6 * scale, 4 * scale);
  builder.close();
  return builder.detach();
}

// Create the crosshair cursor path
SkPath CreateCrosshairPath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  float size = 10 * scale;
  float thickness = 1 * scale;
  // Vertical line
  builder.moveTo(center - thickness, center - size);
  builder.lineTo(center + thickness, center - size);
  builder.lineTo(center + thickness, center + size);
  builder.lineTo(center - thickness, center + size);
  builder.close();
  // Horizontal line
  builder.moveTo(center - size, center - thickness);
  builder.lineTo(center + size, center - thickness);
  builder.lineTo(center + size, center + thickness);
  builder.lineTo(center - size, center + thickness);
  builder.close();
  return builder.detach();
}

// Create the move cursor path (four-way arrows)
SkPath CreateMovePath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  // Up arrow
  builder.moveTo(center, 2 * scale);
  builder.lineTo(center + 4 * scale, 6 * scale);
  builder.lineTo(center + 2 * scale, 6 * scale);
  builder.lineTo(center + 2 * scale, 10 * scale);
  builder.lineTo(center - 2 * scale, 10 * scale);
  builder.lineTo(center - 2 * scale, 6 * scale);
  builder.lineTo(center - 4 * scale, 6 * scale);
  builder.close();
  // Down arrow
  builder.moveTo(center, 22 * scale);
  builder.lineTo(center - 4 * scale, 18 * scale);
  builder.lineTo(center - 2 * scale, 18 * scale);
  builder.lineTo(center - 2 * scale, 14 * scale);
  builder.lineTo(center + 2 * scale, 14 * scale);
  builder.lineTo(center + 2 * scale, 18 * scale);
  builder.lineTo(center + 4 * scale, 18 * scale);
  builder.close();
  // Left arrow
  builder.moveTo(2 * scale, center);
  builder.lineTo(6 * scale, center - 4 * scale);
  builder.lineTo(6 * scale, center - 2 * scale);
  builder.lineTo(10 * scale, center - 2 * scale);
  builder.lineTo(10 * scale, center + 2 * scale);
  builder.lineTo(6 * scale, center + 2 * scale);
  builder.lineTo(6 * scale, center + 4 * scale);
  builder.close();
  // Right arrow
  builder.moveTo(22 * scale, center);
  builder.lineTo(18 * scale, center + 4 * scale);
  builder.lineTo(18 * scale, center + 2 * scale);
  builder.lineTo(14 * scale, center + 2 * scale);
  builder.lineTo(14 * scale, center - 2 * scale);
  builder.lineTo(18 * scale, center - 2 * scale);
  builder.lineTo(18 * scale, center - 4 * scale);
  builder.close();
  return builder.detach();
}

// Create the not-allowed cursor path (circle with slash)
SkPath CreateNotAllowedPath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  float outer_radius = 9 * scale;
  float inner_radius = 7 * scale;
  // Outer and inner circles for the ring
  builder.addCircle(center, center, outer_radius, SkPathDirection::kCW);
  builder.addCircle(center, center, inner_radius, SkPathDirection::kCCW);
  // Diagonal slash
  builder.moveTo(center - 5 * scale, center - 5 * scale);
  builder.lineTo(center - 3 * scale, center - 5 * scale);
  builder.lineTo(center + 5 * scale, center + 5 * scale);
  builder.lineTo(center + 3 * scale, center + 5 * scale);
  builder.close();
  return builder.detach();
}

// Create north-south resize cursor
SkPath CreateNsResizePath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  // Up arrow
  builder.moveTo(center, 2 * scale);
  builder.lineTo(center + 5 * scale, 7 * scale);
  builder.lineTo(center + 2 * scale, 7 * scale);
  builder.lineTo(center + 2 * scale, 17 * scale);
  builder.lineTo(center + 5 * scale, 17 * scale);
  builder.lineTo(center, 22 * scale);
  builder.lineTo(center - 5 * scale, 17 * scale);
  builder.lineTo(center - 2 * scale, 17 * scale);
  builder.lineTo(center - 2 * scale, 7 * scale);
  builder.lineTo(center - 5 * scale, 7 * scale);
  builder.close();
  return builder.detach();
}

// Create east-west resize cursor
SkPath CreateEwResizePath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  // Left arrow
  builder.moveTo(2 * scale, center);
  builder.lineTo(7 * scale, center - 5 * scale);
  builder.lineTo(7 * scale, center - 2 * scale);
  builder.lineTo(17 * scale, center - 2 * scale);
  builder.lineTo(17 * scale, center - 5 * scale);
  builder.lineTo(22 * scale, center);
  builder.lineTo(17 * scale, center + 5 * scale);
  builder.lineTo(17 * scale, center + 2 * scale);
  builder.lineTo(7 * scale, center + 2 * scale);
  builder.lineTo(7 * scale, center + 5 * scale);
  builder.close();
  return builder.detach();
}

// Create northeast-southwest resize cursor
SkPath CreateNeswResizePath(float scale) {
  SkPathBuilder builder;
  // NE arrow
  builder.moveTo(18 * scale, 2 * scale);
  builder.lineTo(22 * scale, 2 * scale);
  builder.lineTo(22 * scale, 6 * scale);
  builder.lineTo(19 * scale, 6 * scale);
  builder.lineTo(19 * scale, 8 * scale);
  builder.lineTo(8 * scale, 19 * scale);
  builder.lineTo(6 * scale, 19 * scale);
  builder.lineTo(6 * scale, 22 * scale);
  builder.lineTo(2 * scale, 22 * scale);
  builder.lineTo(2 * scale, 18 * scale);
  builder.lineTo(5 * scale, 18 * scale);
  builder.lineTo(5 * scale, 16 * scale);
  builder.lineTo(16 * scale, 5 * scale);
  builder.lineTo(18 * scale, 5 * scale);
  builder.close();
  return builder.detach();
}

// Create northwest-southeast resize cursor
SkPath CreateNwseResizePath(float scale) {
  SkPathBuilder builder;
  // NW arrow
  builder.moveTo(6 * scale, 2 * scale);
  builder.lineTo(2 * scale, 2 * scale);
  builder.lineTo(2 * scale, 6 * scale);
  builder.lineTo(5 * scale, 6 * scale);
  builder.lineTo(5 * scale, 8 * scale);
  builder.lineTo(16 * scale, 19 * scale);
  builder.lineTo(18 * scale, 19 * scale);
  builder.lineTo(18 * scale, 22 * scale);
  builder.lineTo(22 * scale, 22 * scale);
  builder.lineTo(22 * scale, 18 * scale);
  builder.lineTo(19 * scale, 18 * scale);
  builder.lineTo(19 * scale, 16 * scale);
  builder.lineTo(8 * scale, 5 * scale);
  builder.lineTo(6 * scale, 5 * scale);
  builder.close();
  return builder.detach();
}

// Create wait/hourglass cursor
SkPath CreateWaitPath(float scale) {
  SkPathBuilder builder;
  // Hourglass shape
  builder.moveTo(4 * scale, 2 * scale);
  builder.lineTo(20 * scale, 2 * scale);
  builder.lineTo(20 * scale, 4 * scale);
  builder.lineTo(14 * scale, 10 * scale);
  builder.lineTo(14 * scale, 14 * scale);
  builder.lineTo(20 * scale, 20 * scale);
  builder.lineTo(20 * scale, 22 * scale);
  builder.lineTo(4 * scale, 22 * scale);
  builder.lineTo(4 * scale, 20 * scale);
  builder.lineTo(10 * scale, 14 * scale);
  builder.lineTo(10 * scale, 10 * scale);
  builder.lineTo(4 * scale, 4 * scale);
  builder.close();
  return builder.detach();
}

// Create help cursor (arrow with question mark)
SkPath CreateHelpPath(float scale) {
  SkPath path = CreateArrowPath(scale);
  SkPathBuilder builder;
  builder.addPath(path);
  // Add question mark circle
  float qx = 16 * scale;
  float qy = 16 * scale;
  builder.addCircle(qx, qy, 6 * scale, SkPathDirection::kCW);
  builder.addCircle(qx, qy, 4 * scale, SkPathDirection::kCCW);
  return builder.detach();
}

// Create grab cursor (open hand with spread fingers)
SkPath CreateGrabPath(float scale) {
  SkPathBuilder builder;
  // Five fingers at top
  // Index finger
  builder.moveTo(6 * scale, 3 * scale);
  builder.lineTo(8 * scale, 3 * scale);
  builder.lineTo(8 * scale, 10 * scale);
  builder.lineTo(6 * scale, 10 * scale);
  builder.close();
  // Middle finger
  builder.moveTo(9 * scale, 2 * scale);
  builder.lineTo(11 * scale, 2 * scale);
  builder.lineTo(11 * scale, 10 * scale);
  builder.lineTo(9 * scale, 10 * scale);
  builder.close();
  // Ring finger
  builder.moveTo(12 * scale, 3 * scale);
  builder.lineTo(14 * scale, 3 * scale);
  builder.lineTo(14 * scale, 10 * scale);
  builder.lineTo(12 * scale, 10 * scale);
  builder.close();
  // Pinky finger
  builder.moveTo(15 * scale, 4 * scale);
  builder.lineTo(17 * scale, 4 * scale);
  builder.lineTo(17 * scale, 10 * scale);
  builder.lineTo(15 * scale, 10 * scale);
  builder.close();
  // Palm body connecting fingers
  builder.moveTo(4 * scale, 10 * scale);
  builder.lineTo(19 * scale, 10 * scale);
  builder.lineTo(19 * scale, 18 * scale);
  builder.lineTo(17 * scale, 20 * scale);
  builder.lineTo(5 * scale, 20 * scale);
  builder.lineTo(3 * scale, 18 * scale);
  builder.lineTo(3 * scale, 13 * scale);
  builder.lineTo(4 * scale, 13 * scale);
  builder.close();
  // Thumb extending left
  builder.moveTo(3 * scale, 13 * scale);
  builder.lineTo(4 * scale, 10 * scale);
  builder.lineTo(3 * scale, 10 * scale);
  builder.lineTo(1 * scale, 12 * scale);
  builder.lineTo(1 * scale, 14 * scale);
  builder.close();
  return builder.detach();
}

// Create grabbing cursor (closed/clenched hand)
SkPath CreateGrabbingPath(float scale) {
  SkPathBuilder builder;
  // Curled fingers at top (shorter, horizontal bumps)
  builder.moveTo(5 * scale, 7 * scale);
  builder.lineTo(7 * scale, 5 * scale);
  builder.lineTo(9 * scale, 5 * scale);
  builder.lineTo(9 * scale, 7 * scale);
  builder.lineTo(10 * scale, 5 * scale);
  builder.lineTo(12 * scale, 5 * scale);
  builder.lineTo(12 * scale, 7 * scale);
  builder.lineTo(13 * scale, 5 * scale);
  builder.lineTo(15 * scale, 5 * scale);
  builder.lineTo(15 * scale, 7 * scale);
  builder.lineTo(16 * scale, 6 * scale);
  builder.lineTo(18 * scale, 6 * scale);
  builder.lineTo(19 * scale, 8 * scale);
  // Fist body
  builder.lineTo(19 * scale, 17 * scale);
  builder.lineTo(17 * scale, 20 * scale);
  builder.lineTo(6 * scale, 20 * scale);
  builder.lineTo(4 * scale, 17 * scale);
  builder.lineTo(3 * scale, 14 * scale);
  builder.lineTo(1 * scale, 12 * scale);
  builder.lineTo(1 * scale, 10 * scale);
  builder.lineTo(3 * scale, 9 * scale);
  builder.lineTo(5 * scale, 9 * scale);
  builder.close();
  return builder.detach();
}

// Create cell cursor (thin plus/cross for spreadsheet selection)
SkPath CreateCellPath(float scale) {
  SkPathBuilder builder;
  float center = 12 * scale;
  float size = 9 * scale;
  float half = 0.5f * scale;
  // Vertical line
  builder.moveTo(center - half, center - size);
  builder.lineTo(center + half, center - size);
  builder.lineTo(center + half, center + size);
  builder.lineTo(center - half, center + size);
  builder.close();
  // Horizontal line
  builder.moveTo(center - size, center - half);
  builder.lineTo(center + size, center - half);
  builder.lineTo(center + size, center + half);
  builder.lineTo(center - size, center + half);
  builder.close();
  return builder.detach();
}

// Create zoom-in cursor (magnifying glass with + inside)
SkPath CreateZoomInPath(float scale) {
  SkPathBuilder builder;
  float cx = 9 * scale;
  float cy = 9 * scale;
  float outer_r = 7 * scale;
  float inner_r = 5.5f * scale;
  // Lens ring
  builder.addCircle(cx, cy, outer_r, SkPathDirection::kCW);
  builder.addCircle(cx, cy, inner_r, SkPathDirection::kCCW);
  // Handle (diagonal line from lens to bottom-right)
  builder.moveTo(14 * scale, 13 * scale);
  builder.lineTo(16 * scale, 13 * scale);
  builder.lineTo(22 * scale, 19 * scale);
  builder.lineTo(22 * scale, 22 * scale);
  builder.lineTo(19 * scale, 22 * scale);
  builder.lineTo(13 * scale, 16 * scale);
  builder.lineTo(13 * scale, 14 * scale);
  builder.close();
  // Plus sign inside lens (horizontal)
  builder.moveTo(cx - 3 * scale, cy - 0.5f * scale);
  builder.lineTo(cx + 3 * scale, cy - 0.5f * scale);
  builder.lineTo(cx + 3 * scale, cy + 0.5f * scale);
  builder.lineTo(cx - 3 * scale, cy + 0.5f * scale);
  builder.close();
  // Plus sign inside lens (vertical)
  builder.moveTo(cx - 0.5f * scale, cy - 3 * scale);
  builder.lineTo(cx + 0.5f * scale, cy - 3 * scale);
  builder.lineTo(cx + 0.5f * scale, cy + 3 * scale);
  builder.lineTo(cx - 0.5f * scale, cy + 3 * scale);
  builder.close();
  return builder.detach();
}

// Create zoom-out cursor (magnifying glass with - inside)
SkPath CreateZoomOutPath(float scale) {
  SkPathBuilder builder;
  float cx = 9 * scale;
  float cy = 9 * scale;
  float outer_r = 7 * scale;
  float inner_r = 5.5f * scale;
  // Lens ring
  builder.addCircle(cx, cy, outer_r, SkPathDirection::kCW);
  builder.addCircle(cx, cy, inner_r, SkPathDirection::kCCW);
  // Handle
  builder.moveTo(14 * scale, 13 * scale);
  builder.lineTo(16 * scale, 13 * scale);
  builder.lineTo(22 * scale, 19 * scale);
  builder.lineTo(22 * scale, 22 * scale);
  builder.lineTo(19 * scale, 22 * scale);
  builder.lineTo(13 * scale, 16 * scale);
  builder.lineTo(13 * scale, 14 * scale);
  builder.close();
  // Minus sign inside lens (horizontal only)
  builder.moveTo(cx - 3 * scale, cy - 0.5f * scale);
  builder.lineTo(cx + 3 * scale, cy - 0.5f * scale);
  builder.lineTo(cx + 3 * scale, cy + 0.5f * scale);
  builder.lineTo(cx - 3 * scale, cy + 0.5f * scale);
  builder.close();
  return builder.detach();
}

// Create context menu cursor (arrow with menu lines)
SkPath CreateContextMenuPath(float scale) {
  SkPath path = CreateArrowPath(scale);
  SkPathBuilder builder;
  builder.addPath(path);
  // Three horizontal menu lines at bottom-right
  float mx = 14 * scale;
  float my = 14 * scale;
  float line_w = 7 * scale;
  float line_h = 1 * scale;
  float spacing = 3 * scale;
  for (int i = 0; i < 3; ++i) {
    float y = my + i * spacing;
    builder.moveTo(mx, y);
    builder.lineTo(mx + line_w, y);
    builder.lineTo(mx + line_w, y + line_h);
    builder.lineTo(mx, y + line_h);
    builder.close();
  }
  return builder.detach();
}

// Create alias cursor (arrow with curved shortcut arrow)
SkPath CreateAliasPath(float scale) {
  SkPath path = CreateArrowPath(scale);
  SkPathBuilder builder;
  builder.addPath(path);
  // Small curved arrow badge at bottom-right
  // Arrow shaft curving from right to up
  builder.moveTo(20 * scale, 20 * scale);
  builder.lineTo(14 * scale, 20 * scale);
  builder.lineTo(14 * scale, 15 * scale);
  builder.lineTo(16 * scale, 15 * scale);
  builder.lineTo(16 * scale, 18 * scale);
  builder.lineTo(20 * scale, 18 * scale);
  builder.close();
  // Arrowhead pointing up
  builder.moveTo(12 * scale, 15 * scale);
  builder.lineTo(15 * scale, 12 * scale);
  builder.lineTo(18 * scale, 15 * scale);
  builder.lineTo(16 * scale, 15 * scale);
  builder.lineTo(15 * scale, 14 * scale);
  builder.lineTo(14 * scale, 15 * scale);
  builder.close();
  return builder.detach();
}

// Create copy cursor (arrow with + badge)
SkPath CreateCopyPath(float scale) {
  SkPath path = CreateArrowPath(scale);
  SkPathBuilder builder;
  builder.addPath(path);
  // Plus sign badge at bottom-right
  float px = 17 * scale;
  float py = 17 * scale;
  // Horizontal bar
  builder.moveTo(px - 3 * scale, py - 1 * scale);
  builder.lineTo(px + 3 * scale, py - 1 * scale);
  builder.lineTo(px + 3 * scale, py + 1 * scale);
  builder.lineTo(px - 3 * scale, py + 1 * scale);
  builder.close();
  // Vertical bar
  builder.moveTo(px - 1 * scale, py - 3 * scale);
  builder.lineTo(px + 1 * scale, py - 3 * scale);
  builder.lineTo(px + 1 * scale, py + 3 * scale);
  builder.lineTo(px - 1 * scale, py + 3 * scale);
  builder.close();
  return builder.detach();
}

// Create vertical text cursor (horizontal I-beam, rotated 90 degrees)
SkPath CreateVerticalTextPath(float scale) {
  SkPathBuilder builder;
  // Left serif
  builder.moveTo(2 * scale, 6 * scale);
  builder.lineTo(4 * scale, 6 * scale);
  builder.lineTo(4 * scale, 7 * scale);
  builder.lineTo(11 * scale, 7 * scale);
  builder.lineTo(11 * scale, 6 * scale);
  builder.lineTo(13 * scale, 6 * scale);
  builder.lineTo(13 * scale, 10 * scale);
  builder.lineTo(11 * scale, 10 * scale);
  builder.lineTo(11 * scale, 9 * scale);
  builder.lineTo(4 * scale, 9 * scale);
  builder.lineTo(4 * scale, 10 * scale);
  builder.lineTo(2 * scale, 10 * scale);
  builder.close();
  // Right serif
  builder.moveTo(14 * scale, 6 * scale);
  builder.lineTo(16 * scale, 6 * scale);
  builder.lineTo(16 * scale, 7 * scale);
  builder.lineTo(20 * scale, 7 * scale);
  builder.lineTo(20 * scale, 6 * scale);
  builder.lineTo(22 * scale, 6 * scale);
  builder.lineTo(22 * scale, 10 * scale);
  builder.lineTo(20 * scale, 10 * scale);
  builder.lineTo(20 * scale, 9 * scale);
  builder.lineTo(16 * scale, 9 * scale);
  builder.lineTo(16 * scale, 10 * scale);
  builder.lineTo(14 * scale, 10 * scale);
  builder.close();
  // Center bar connecting the two serifs
  builder.moveTo(11 * scale, 7 * scale);
  builder.lineTo(14 * scale, 7 * scale);
  builder.lineTo(14 * scale, 9 * scale);
  builder.lineTo(11 * scale, 9 * scale);
  builder.close();
  return builder.detach();
}

void DrawCursorPath(cc::PaintCanvas* canvas,
                    const SkPath& path,
                    const gfx::PointF& position,
                    float scale) {
  canvas->save();
  canvas->translate(position.x(), position.y());

  // Draw white outline/stroke for visibility
  cc::PaintFlags stroke_flags;
  stroke_flags.setColor(SK_ColorWHITE);
  stroke_flags.setStyle(cc::PaintFlags::kStroke_Style);
  stroke_flags.setStrokeWidth(2.0f * scale);
  stroke_flags.setAntiAlias(true);
  canvas->drawPath(path, stroke_flags);

  // Draw black fill
  cc::PaintFlags fill_flags;
  fill_flags.setColor(SK_ColorBLACK);
  fill_flags.setStyle(cc::PaintFlags::kFill_Style);
  fill_flags.setAntiAlias(true);
  canvas->drawPath(path, fill_flags);

  canvas->restore();
}

}  // namespace

// static
void InspectorCursorDrawer::DrawCursor(
    cc::PaintCanvas* canvas,
    ui::mojom::blink::CursorType cursor_type,
    const gfx::PointF& position,
    float scale) {
  if (!canvas) {
    return;
  }

  // Scale down cursor to 70% so it matches the system cursor size more closely.
  constexpr float kCursorScale = 0.7f;
  float cursor_scale = scale * kCursorScale;

  // Get the hotspot and adjust position so hotspot is at the click point
  gfx::PointF hotspot = GetCursorHotspot(cursor_type);
  gfx::PointF adjusted_pos(position.x() - hotspot.x() * cursor_scale,
                           position.y() - hotspot.y() * cursor_scale);

  SkPath path;

  switch (cursor_type) {
    case ui::mojom::blink::CursorType::kPointer:
    case ui::mojom::blink::CursorType::kNone:
      path = CreateArrowPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kHand:
      path = CreateHandPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kIBeam:
      path = CreateIBeamPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kCross:
      path = CreateCrosshairPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kMove:
    case ui::mojom::blink::CursorType::kMiddlePanning:
    case ui::mojom::blink::CursorType::kMiddlePanningVertical:
    case ui::mojom::blink::CursorType::kMiddlePanningHorizontal:
      path = CreateMovePath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kNotAllowed:
    case ui::mojom::blink::CursorType::kNoDrop:
      path = CreateNotAllowedPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kNorthResize:
    case ui::mojom::blink::CursorType::kSouthResize:
    case ui::mojom::blink::CursorType::kNorthSouthResize:
    case ui::mojom::blink::CursorType::kRowResize:
      path = CreateNsResizePath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kEastResize:
    case ui::mojom::blink::CursorType::kWestResize:
    case ui::mojom::blink::CursorType::kEastWestResize:
    case ui::mojom::blink::CursorType::kColumnResize:
      path = CreateEwResizePath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kNorthEastResize:
    case ui::mojom::blink::CursorType::kSouthWestResize:
    case ui::mojom::blink::CursorType::kNorthEastSouthWestResize:
      path = CreateNeswResizePath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kNorthWestResize:
    case ui::mojom::blink::CursorType::kSouthEastResize:
    case ui::mojom::blink::CursorType::kNorthWestSouthEastResize:
      path = CreateNwseResizePath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kWait:
    case ui::mojom::blink::CursorType::kProgress:
      path = CreateWaitPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kHelp:
      path = CreateHelpPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kGrab:
      path = CreateGrabPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kGrabbing:
      path = CreateGrabbingPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kCell:
      path = CreateCellPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kZoomIn:
      path = CreateZoomInPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kZoomOut:
      path = CreateZoomOutPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kContextMenu:
      path = CreateContextMenuPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kAlias:
      path = CreateAliasPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kCopy:
      path = CreateCopyPath(cursor_scale);
      break;

    case ui::mojom::blink::CursorType::kVerticalText:
      path = CreateVerticalTextPath(cursor_scale);
      break;

    // For other cursor types, fall back to arrow
    default:
      path = CreateArrowPath(cursor_scale);
      break;
  }

  DrawCursorPath(canvas, path, adjusted_pos, cursor_scale);
}

// static
String InspectorCursorDrawer::CursorTypeName(
    ui::mojom::blink::CursorType cursor_type) {
  switch (cursor_type) {
    case ui::mojom::blink::CursorType::kPointer:
      return "pointer";
    case ui::mojom::blink::CursorType::kCross:
      return "crosshair";
    case ui::mojom::blink::CursorType::kHand:
      return "hand";
    case ui::mojom::blink::CursorType::kIBeam:
      return "text";
    case ui::mojom::blink::CursorType::kWait:
      return "wait";
    case ui::mojom::blink::CursorType::kHelp:
      return "help";
    case ui::mojom::blink::CursorType::kEastResize:
      return "e-resize";
    case ui::mojom::blink::CursorType::kNorthResize:
      return "n-resize";
    case ui::mojom::blink::CursorType::kNorthEastResize:
      return "ne-resize";
    case ui::mojom::blink::CursorType::kNorthWestResize:
      return "nw-resize";
    case ui::mojom::blink::CursorType::kSouthResize:
      return "s-resize";
    case ui::mojom::blink::CursorType::kSouthEastResize:
      return "se-resize";
    case ui::mojom::blink::CursorType::kSouthWestResize:
      return "sw-resize";
    case ui::mojom::blink::CursorType::kWestResize:
      return "w-resize";
    case ui::mojom::blink::CursorType::kNorthSouthResize:
      return "ns-resize";
    case ui::mojom::blink::CursorType::kEastWestResize:
      return "ew-resize";
    case ui::mojom::blink::CursorType::kNorthEastSouthWestResize:
      return "nesw-resize";
    case ui::mojom::blink::CursorType::kNorthWestSouthEastResize:
      return "nwse-resize";
    case ui::mojom::blink::CursorType::kColumnResize:
      return "col-resize";
    case ui::mojom::blink::CursorType::kRowResize:
      return "row-resize";
    case ui::mojom::blink::CursorType::kMiddlePanning:
      return "all-scroll";
    case ui::mojom::blink::CursorType::kMiddlePanningVertical:
      return "all-scroll";
    case ui::mojom::blink::CursorType::kMiddlePanningHorizontal:
      return "all-scroll";
    case ui::mojom::blink::CursorType::kEastPanning:
      return "e-resize";
    case ui::mojom::blink::CursorType::kNorthPanning:
      return "n-resize";
    case ui::mojom::blink::CursorType::kNorthEastPanning:
      return "ne-resize";
    case ui::mojom::blink::CursorType::kNorthWestPanning:
      return "nw-resize";
    case ui::mojom::blink::CursorType::kSouthPanning:
      return "s-resize";
    case ui::mojom::blink::CursorType::kSouthEastPanning:
      return "se-resize";
    case ui::mojom::blink::CursorType::kSouthWestPanning:
      return "sw-resize";
    case ui::mojom::blink::CursorType::kWestPanning:
      return "w-resize";
    case ui::mojom::blink::CursorType::kMove:
      return "move";
    case ui::mojom::blink::CursorType::kVerticalText:
      return "vertical-text";
    case ui::mojom::blink::CursorType::kCell:
      return "cell";
    case ui::mojom::blink::CursorType::kContextMenu:
      return "context-menu";
    case ui::mojom::blink::CursorType::kAlias:
      return "alias";
    case ui::mojom::blink::CursorType::kProgress:
      return "progress";
    case ui::mojom::blink::CursorType::kNoDrop:
      return "no-drop";
    case ui::mojom::blink::CursorType::kCopy:
      return "copy";
    case ui::mojom::blink::CursorType::kNone:
      return "none";
    case ui::mojom::blink::CursorType::kNotAllowed:
      return "not-allowed";
    case ui::mojom::blink::CursorType::kZoomIn:
      return "zoom-in";
    case ui::mojom::blink::CursorType::kZoomOut:
      return "zoom-out";
    case ui::mojom::blink::CursorType::kGrab:
      return "grab";
    case ui::mojom::blink::CursorType::kGrabbing:
      return "grabbing";
    case ui::mojom::blink::CursorType::kCustom:
      return "custom";
    default:
      return "default";
  }
}

// static
gfx::PointF InspectorCursorDrawer::GetCursorHotspot(
    ui::mojom::blink::CursorType cursor_type) {
  switch (cursor_type) {
    case ui::mojom::blink::CursorType::kPointer:
    case ui::mojom::blink::CursorType::kNone:
      // Arrow tip is at top-left
      return gfx::PointF(1.0f, 1.0f);

    case ui::mojom::blink::CursorType::kHand:
      // Fingertip is at top of index finger
      return gfx::PointF(8.0f, 1.0f);

    case ui::mojom::blink::CursorType::kIBeam:
      // Center of I-beam
      return gfx::PointF(8.0f, 12.0f);

    case ui::mojom::blink::CursorType::kCross:
      // Center of crosshair
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::blink::CursorType::kMove:
    case ui::mojom::blink::CursorType::kMiddlePanning:
    case ui::mojom::blink::CursorType::kMiddlePanningVertical:
    case ui::mojom::blink::CursorType::kMiddlePanningHorizontal:
      // Center of four-way arrows
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::blink::CursorType::kNotAllowed:
    case ui::mojom::blink::CursorType::kNoDrop:
      // Center of circle
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::blink::CursorType::kNorthResize:
    case ui::mojom::blink::CursorType::kSouthResize:
    case ui::mojom::blink::CursorType::kNorthSouthResize:
    case ui::mojom::blink::CursorType::kRowResize:
    case ui::mojom::blink::CursorType::kEastResize:
    case ui::mojom::blink::CursorType::kWestResize:
    case ui::mojom::blink::CursorType::kEastWestResize:
    case ui::mojom::blink::CursorType::kColumnResize:
    case ui::mojom::blink::CursorType::kNorthEastResize:
    case ui::mojom::blink::CursorType::kNorthWestResize:
    case ui::mojom::blink::CursorType::kSouthEastResize:
    case ui::mojom::blink::CursorType::kSouthWestResize:
    case ui::mojom::blink::CursorType::kNorthEastSouthWestResize:
    case ui::mojom::blink::CursorType::kNorthWestSouthEastResize:
      // Center of resize arrows
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::blink::CursorType::kWait:
    case ui::mojom::blink::CursorType::kProgress:
      // Center of hourglass
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::blink::CursorType::kHelp:
      // Arrow tip with question mark offset
      return gfx::PointF(1.0f, 1.0f);

    case ui::mojom::blink::CursorType::kGrab:
    case ui::mojom::blink::CursorType::kGrabbing:
      // Center of palm
      return gfx::PointF(10.0f, 10.0f);

    case ui::mojom::blink::CursorType::kCell:
      // Center of plus
      return gfx::PointF(12.0f, 12.0f);

    case ui::mojom::blink::CursorType::kZoomIn:
    case ui::mojom::blink::CursorType::kZoomOut:
      // Center of lens circle
      return gfx::PointF(9.0f, 9.0f);

    case ui::mojom::blink::CursorType::kContextMenu:
    case ui::mojom::blink::CursorType::kAlias:
    case ui::mojom::blink::CursorType::kCopy:
      // Arrow tip (same as pointer)
      return gfx::PointF(1.0f, 1.0f);

    case ui::mojom::blink::CursorType::kVerticalText:
      // Center of horizontal I-beam
      return gfx::PointF(12.0f, 8.0f);

    default:
      // Default to arrow hotspot
      return gfx::PointF(1.0f, 1.0f);
  }
}

}  // namespace blink
