// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_
#define CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "content/public/browser/popup_interceptor.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/choosers/color_chooser.mojom.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom.h"

namespace abp {

class AbpController;

struct PendingSelectPopup {
  std::string tab_id;
  mojo::Remote<blink::mojom::PopupMenuClient> client;
  std::vector<blink::mojom::MenuItemPtr> items;
  int32_t selected_index;
  bool allow_multiple;
  gfx::Rect bounds;
};

struct PendingColorPicker {
  std::string tab_id;
  mojo::Remote<blink::mojom::ColorChooserClient> client;
  // Hold the receiver to keep the Mojo pipe alive
  mojo::PendingReceiver<blink::mojom::ColorChooser> chooser_receiver;
  SkColor current_color;
  std::vector<blink::mojom::ColorSuggestionPtr> suggestions;
};

class AbpPopupInterceptor : public content::PopupInterceptor {
 public:
  explicit AbpPopupInterceptor(AbpController* controller);
  ~AbpPopupInterceptor() override;

  // content::PopupInterceptor:
  bool OnSelectPopupRequested(
      content::RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
      const gfx::Rect& bounds,
      int32_t selected_item,
      std::vector<blink::mojom::MenuItemPtr> menu_items,
      bool allow_multiple_selection) override;

  bool OnColorChooserRequested(
      content::RenderFrameHost* rfh,
      mojo::PendingReceiver<blink::mojom::ColorChooser> chooser_receiver,
      mojo::PendingRemote<blink::mojom::ColorChooserClient> client,
      SkColor color,
      std::vector<blink::mojom::ColorSuggestionPtr> suggestions) override;

  // Respond to a pending select popup (called by controller on agent response)
  // Returns false if popup_id not found.
  bool RespondToSelectPopup(const std::string& popup_id,
                            const std::vector<int32_t>& indices);
  bool CancelSelectPopup(const std::string& popup_id);

  // Respond to a pending color picker
  bool RespondToColorPicker(const std::string& popup_id, SkColor color);
  bool CancelColorPicker(const std::string& popup_id);

  // Get pending popup info as JSON (for REST/MCP)
  base::Value::Dict GetPendingSelectPopup(const std::string& popup_id) const;
  base::Value::Dict GetPendingColorPicker(const std::string& popup_id) const;
  base::Value::List GetAllPendingPopups() const;

  // Clean up popups for a tab (called on tab close)
  void CleanupForTab(const std::string& tab_id);

 private:
  std::string GenerateSelectPopupId();
  std::string GenerateColorPickerId();

  // Serialize menu items to JSON
  static base::Value::List SerializeMenuItems(
      const std::vector<blink::mojom::MenuItemPtr>& items);

  raw_ptr<AbpController> controller_;
  std::map<std::string, PendingSelectPopup> pending_select_popups_;
  std::map<std::string, PendingColorPicker> pending_color_pickers_;
  int next_select_popup_id_ = 1;
  int next_color_picker_id_ = 1;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_
