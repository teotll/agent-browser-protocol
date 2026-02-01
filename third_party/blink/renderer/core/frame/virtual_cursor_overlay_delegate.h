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
