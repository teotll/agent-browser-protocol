# Native Popup Interception Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Intercept native OS popups (`<select>` dropdowns, color pickers) at the Mojo IPC boundary, suppress native UI, and expose them to agents via REST API + MCP tools (`browser_select_picker`, `browser_color_picker`).

**Architecture:** Delegate pattern — define a `PopupInterceptor` interface in `content/public/browser/`, implement it in `chrome/browser/abp/`, and register it via `WebContents` user data. `RenderFrameHostImpl::ShowPopupMenu()` checks for the interceptor before delegating to platform UI. ABP holds the Mojo remote until the agent responds, then completes the selection.

**Tech Stack:** C++ (Chromium), Mojo IPC, base::Value JSON, ToolBuilder (MCP)

**Design Doc:** `docs/plans/2026-02-22-native-popup-interception-design.md`

---

### Task 1: Add PopupInterceptor interface to content/public/browser

Define the delegate interface that ABP will implement.

**Files:**
- Create: `content/public/browser/popup_interceptor.h`
- Modify: `content/browser/renderer_host/render_frame_host_impl.cc:9308-9364`
- Modify: `content/browser/web_contents/web_contents_impl.cc:8359-8384`

**Step 1: Create the PopupInterceptor interface**

Create `content/public/browser/popup_interceptor.h`:

```cpp
// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_POPUP_INTERCEPTOR_H_
#define CONTENT_PUBLIC_BROWSER_POPUP_INTERCEPTOR_H_

#include <vector>

#include "content/common/content_export.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom-forward.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/geometry/rect.h"

namespace blink::mojom {
class PopupMenuClient;
class ColorChooserClient;
class ColorSuggestion;
}  // namespace blink::mojom

namespace content {

class RenderFrameHost;

// Interface for intercepting native popups before platform UI is shown.
// Embedders (e.g., ABP) implement this to capture select dropdowns and
// color pickers, exposing them to external agents.
class CONTENT_EXPORT PopupInterceptor {
 public:
  virtual ~PopupInterceptor() = default;

  // Called when a <select> element requests a native popup menu.
  // Returns true if the popup was intercepted (native UI is suppressed).
  // The interceptor takes ownership of |popup_client| to send the selection.
  virtual bool OnSelectPopupRequested(
      RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
      const gfx::Rect& bounds,
      int32_t selected_item,
      std::vector<blink::mojom::MenuItemPtr> menu_items,
      bool allow_multiple_selection) = 0;

  // Called when an <input type="color"> requests a native color chooser.
  // Returns true if intercepted (native UI is suppressed).
  virtual bool OnColorChooserRequested(
      RenderFrameHost* rfh,
      mojo::PendingReceiver<blink::mojom::ColorChooser> chooser_receiver,
      mojo::PendingRemote<blink::mojom::ColorChooserClient> client,
      SkColor color,
      std::vector<blink::mojom::ColorSuggestionPtr> suggestions) = 0;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_POPUP_INTERCEPTOR_H_
```

**Step 2: Add WebContents user data accessor**

Add getter/setter to `content/public/browser/web_contents.h` (find the public methods section near other delegate accessors):

```cpp
// Popup interceptor for native popups (select, color).
// Lifetime managed by caller; must outlive the WebContents.
virtual void SetPopupInterceptor(PopupInterceptor* interceptor) = 0;
virtual PopupInterceptor* GetPopupInterceptor() = 0;
```

Implement in `content/browser/web_contents/web_contents_impl.h` (add member) and `.cc`:

```cpp
// In web_contents_impl.h private section:
raw_ptr<PopupInterceptor> popup_interceptor_ = nullptr;

// In web_contents_impl.cc:
void WebContentsImpl::SetPopupInterceptor(PopupInterceptor* interceptor) {
  popup_interceptor_ = interceptor;
}

PopupInterceptor* WebContentsImpl::GetPopupInterceptor() {
  return popup_interceptor_;
}
```

**Step 3: Hook into RenderFrameHostImpl::ShowPopupMenu**

In `content/browser/renderer_host/render_frame_host_impl.cc`, at the top of `ShowPopupMenu()` (after the `#if BUILDFLAG(USE_EXTERNAL_POPUP_MENU)` line, before the inactive document check):

```cpp
void RenderFrameHostImpl::ShowPopupMenu(...) {
#if BUILDFLAG(USE_EXTERNAL_POPUP_MENU)
  // Check for popup interceptor (e.g., ABP agent browser protocol)
  if (auto* wc = GetPage().GetMainDocument().GetWebContents()) {
    if (auto* interceptor = static_cast<WebContentsImpl*>(wc)->GetPopupInterceptor()) {
      if (interceptor->OnSelectPopupRequested(
              this, std::move(popup_client), bounds, selected_item,
              std::move(menu_items), allow_multiple_selection)) {
        return;  // Intercepted, skip native UI
      }
    }
  }

  // ... existing code unchanged ...
```

**Step 4: Hook into WebContentsImpl::OpenColorChooser**

In `content/browser/web_contents/web_contents_impl.cc`, at the top of `OpenColorChooser()`:

```cpp
void WebContentsImpl::OpenColorChooser(...) {
  // Check for popup interceptor (e.g., ABP agent browser protocol)
  if (popup_interceptor_) {
    if (popup_interceptor_->OnColorChooserRequested(
            /* rfh */ nullptr,  // TODO: pass the requesting frame
            std::move(chooser_receiver), std::move(client),
            color, std::move(suggestions))) {
      return;  // Intercepted, skip native UI
    }
  }

  // ... existing code unchanged ...
```

**Step 5: Update BUILD.gn**

Add the new header to `content/public/browser/BUILD.gn` sources list (find the alphabetical position near other .h files):

```gn
"popup_interceptor.h",
```

**Step 6: Build and verify compilation**

Run: `autoninja -C out/Default chrome`
Expected: Compiles without errors. No behavioral change (no interceptor registered yet).

**Step 7: Commit**

```bash
git add content/public/browser/popup_interceptor.h \
       content/public/browser/web_contents.h \
       content/public/browser/BUILD.gn \
       content/browser/web_contents/web_contents_impl.h \
       content/browser/web_contents/web_contents_impl.cc \
       content/browser/renderer_host/render_frame_host_impl.cc
git commit -m "feat(content): add PopupInterceptor delegate for native popup interception"
```

---

### Task 2: Add AbpPopupInterceptor and pending state in AbpController

Implement the `PopupInterceptor` interface in ABP and wire up storage for pending popups.

**Files:**
- Create: `chrome/browser/abp/abp_popup_interceptor.h`
- Create: `chrome/browser/abp/abp_popup_interceptor.cc`
- Modify: `chrome/browser/abp/abp_controller.h` (~line 942, near `pending_file_choosers_`)
- Modify: `chrome/browser/abp/abp_controller.cc` (constructor, tab lifecycle)
- Modify: `chrome/browser/abp/BUILD.gn`

**Step 1: Create AbpPopupInterceptor**

Create `chrome/browser/abp/abp_popup_interceptor.h`:

```cpp
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_
#define CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_

#include <map>
#include <string>
#include <vector>

#include "base/values.h"
#include "content/public/browser/popup_interceptor.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/choosers/color_chooser.mojom.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom.h"

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

#endif  // CHROME_BROWSER_ABP_ABP_POPUP_INTERCEPTOR_H_
```

**Step 2: Create AbpPopupInterceptor implementation**

Create `chrome/browser/abp/abp_popup_interceptor.cc`:

```cpp
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_popup_interceptor.h"

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/abp/abp_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"

AbpPopupInterceptor::AbpPopupInterceptor(AbpController* controller)
    : controller_(controller) {}

AbpPopupInterceptor::~AbpPopupInterceptor() = default;

std::string AbpPopupInterceptor::GenerateSelectPopupId() {
  return base::StringPrintf("sp_%d", next_select_popup_id_++);
}

std::string AbpPopupInterceptor::GenerateColorPickerId() {
  return base::StringPrintf("cp_%d", next_color_picker_id_++);
}

bool AbpPopupInterceptor::OnSelectPopupRequested(
    content::RenderFrameHost* rfh,
    mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
    const gfx::Rect& bounds,
    int32_t selected_item,
    std::vector<blink::mojom::MenuItemPtr> menu_items,
    bool allow_multiple_selection) {
  if (!controller_)
    return false;

  // Get tab_id from the DevToolsAgentHost associated with this frame's
  // WebContents (ABP uses DevToolsAgentHost IDs as tab IDs)
  std::string tab_id;
  if (rfh) {
    auto* wc = content::WebContents::FromRenderFrameHost(rfh);
    if (wc) {
      tab_id = controller_->GetTabIdForWebContents(wc);
    }
  }
  if (tab_id.empty())
    return false;

  std::string popup_id = GenerateSelectPopupId();

  VLOG(1) << "ABP: Select popup intercepted, id=" << popup_id
          << " tab=" << tab_id
          << " items=" << menu_items.size();

  // Serialize event data before moving items
  base::Value::Dict event_data;
  event_data.Set("type", "select_open");
  event_data.Set("id", popup_id);
  event_data.Set("tab_id", tab_id);
  event_data.Set("allow_multiple_selection", allow_multiple_selection);
  event_data.Set("selected_index", selected_item);

  base::Value::Dict bounds_dict;
  bounds_dict.Set("x", bounds.x());
  bounds_dict.Set("y", bounds.y());
  bounds_dict.Set("width", bounds.width());
  bounds_dict.Set("height", bounds.height());
  event_data.Set("bounds", std::move(bounds_dict));

  event_data.Set("items", SerializeMenuItems(menu_items));

  // Store pending state
  PendingSelectPopup pending;
  pending.tab_id = tab_id;
  pending.client.Bind(std::move(popup_client));
  pending.items = std::move(menu_items);
  pending.selected_index = selected_item;
  pending.allow_multiple = allow_multiple_selection;
  pending.bounds = bounds;

  // Set disconnect handler to clean up if renderer goes away
  pending.client.set_disconnect_handler(base::BindOnce(
      [](AbpPopupInterceptor* self, std::string id) {
        VLOG(1) << "ABP: Select popup " << id << " disconnected";
        self->pending_select_popups_.erase(id);
      },
      base::Unretained(this), popup_id));

  pending_select_popups_[popup_id] = std::move(pending);

  // Emit event (collected by event collector for action responses)
  controller_->EmitPopupEvent("select_open", std::move(event_data));

  return true;  // Intercepted — suppress native UI
}

bool AbpPopupInterceptor::OnColorChooserRequested(
    content::RenderFrameHost* rfh,
    mojo::PendingReceiver<blink::mojom::ColorChooser> chooser_receiver,
    mojo::PendingRemote<blink::mojom::ColorChooserClient> client,
    SkColor color,
    std::vector<blink::mojom::ColorSuggestionPtr> suggestions) {
  if (!controller_)
    return false;

  // For color chooser, rfh may be null (passed through WebContentsImpl).
  // We need to get the tab_id from the active tab.
  // TODO: Thread the RenderFrameHost through OpenColorChooser
  std::string tab_id = controller_->GetActiveTabId();
  if (tab_id.empty())
    return false;

  std::string popup_id = GenerateColorPickerId();

  VLOG(1) << "ABP: Color picker intercepted, id=" << popup_id
          << " tab=" << tab_id;

  // Serialize event data
  base::Value::Dict event_data;
  event_data.Set("type", "color_picker_open");
  event_data.Set("id", popup_id);
  event_data.Set("tab_id", tab_id);

  // Convert SkColor to hex string
  event_data.Set("current_color",
      base::StringPrintf("#%02x%02x%02x",
          SkColorGetR(color), SkColorGetG(color), SkColorGetB(color)));

  base::Value::List suggestions_list;
  for (const auto& s : suggestions) {
    base::Value::Dict sd;
    sd.Set("color",
        base::StringPrintf("#%02x%02x%02x",
            SkColorGetR(s->color), SkColorGetG(s->color),
            SkColorGetB(s->color)));
    sd.Set("label", s->label);
    suggestions_list.Append(std::move(sd));
  }
  event_data.Set("suggestions", std::move(suggestions_list));

  // Store pending state
  PendingColorPicker pending;
  pending.tab_id = tab_id;
  pending.client.Bind(std::move(client));
  pending.chooser_receiver = std::move(chooser_receiver);
  pending.current_color = color;
  pending.suggestions = std::move(suggestions);

  pending.client.set_disconnect_handler(base::BindOnce(
      [](AbpPopupInterceptor* self, std::string id) {
        VLOG(1) << "ABP: Color picker " << id << " disconnected";
        self->pending_color_pickers_.erase(id);
      },
      base::Unretained(this), popup_id));

  pending_color_pickers_[popup_id] = std::move(pending);

  controller_->EmitPopupEvent("color_picker_open", std::move(event_data));

  return true;
}

bool AbpPopupInterceptor::RespondToSelectPopup(
    const std::string& popup_id,
    const std::vector<int32_t>& indices) {
  auto it = pending_select_popups_.find(popup_id);
  if (it == pending_select_popups_.end())
    return false;

  it->second.client->DidAcceptIndices(indices);
  pending_select_popups_.erase(it);
  return true;
}

bool AbpPopupInterceptor::CancelSelectPopup(const std::string& popup_id) {
  auto it = pending_select_popups_.find(popup_id);
  if (it == pending_select_popups_.end())
    return false;

  it->second.client->DidCancel();
  pending_select_popups_.erase(it);
  return true;
}

bool AbpPopupInterceptor::RespondToColorPicker(const std::string& popup_id,
                                                SkColor color) {
  auto it = pending_color_pickers_.find(popup_id);
  if (it == pending_color_pickers_.end())
    return false;

  it->second.client->DidChooseColor(color);
  pending_color_pickers_.erase(it);
  return true;
}

bool AbpPopupInterceptor::CancelColorPicker(const std::string& popup_id) {
  auto it = pending_color_pickers_.find(popup_id);
  if (it == pending_color_pickers_.end())
    return false;

  // Dropping the Mojo remote signals cancellation
  pending_color_pickers_.erase(it);
  return true;
}

base::Value::Dict AbpPopupInterceptor::GetPendingSelectPopup(
    const std::string& popup_id) const {
  auto it = pending_select_popups_.find(popup_id);
  if (it == pending_select_popups_.end())
    return base::Value::Dict();

  base::Value::Dict result;
  result.Set("type", "select_open");
  result.Set("id", popup_id);
  result.Set("tab_id", it->second.tab_id);
  result.Set("allow_multiple_selection", it->second.allow_multiple);
  result.Set("selected_index", it->second.selected_index);

  base::Value::Dict bounds;
  bounds.Set("x", it->second.bounds.x());
  bounds.Set("y", it->second.bounds.y());
  bounds.Set("width", it->second.bounds.width());
  bounds.Set("height", it->second.bounds.height());
  result.Set("bounds", std::move(bounds));

  result.Set("items", SerializeMenuItems(it->second.items));
  return result;
}

base::Value::Dict AbpPopupInterceptor::GetPendingColorPicker(
    const std::string& popup_id) const {
  auto it = pending_color_pickers_.find(popup_id);
  if (it == pending_color_pickers_.end())
    return base::Value::Dict();

  base::Value::Dict result;
  result.Set("type", "color_picker_open");
  result.Set("id", popup_id);
  result.Set("tab_id", it->second.tab_id);
  result.Set("current_color",
      base::StringPrintf("#%02x%02x%02x",
          SkColorGetR(it->second.current_color),
          SkColorGetG(it->second.current_color),
          SkColorGetB(it->second.current_color)));
  return result;
}

base::Value::List AbpPopupInterceptor::GetAllPendingPopups() const {
  base::Value::List result;
  for (const auto& [id, _] : pending_select_popups_) {
    result.Append(GetPendingSelectPopup(id));
  }
  for (const auto& [id, _] : pending_color_pickers_) {
    result.Append(GetPendingColorPicker(id));
  }
  return result;
}

void AbpPopupInterceptor::CleanupForTab(const std::string& tab_id) {
  // Cancel and remove all pending popups for this tab
  for (auto it = pending_select_popups_.begin();
       it != pending_select_popups_.end();) {
    if (it->second.tab_id == tab_id) {
      // DidCancel not needed — Mojo will disconnect on tab close
      it = pending_select_popups_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = pending_color_pickers_.begin();
       it != pending_color_pickers_.end();) {
    if (it->second.tab_id == tab_id) {
      it = pending_color_pickers_.erase(it);
    } else {
      ++it;
    }
  }
}

// static
base::Value::List AbpPopupInterceptor::SerializeMenuItems(
    const std::vector<blink::mojom::MenuItemPtr>& items) {
  base::Value::List list;
  int index = 0;
  for (const auto& item : items) {
    base::Value::Dict d;
    d.Set("index", index++);

    switch (item->type) {
      case blink::mojom::MenuItem::Type::kOption:
        d.Set("type", "option");
        break;
      case blink::mojom::MenuItem::Type::kCheckableOption:
        d.Set("type", "checkable_option");
        break;
      case blink::mojom::MenuItem::Type::kGroup:
        d.Set("type", "group");
        break;
      case blink::mojom::MenuItem::Type::kSeparator:
        d.Set("type", "separator");
        break;
      case blink::mojom::MenuItem::Type::kSubMenu:
        d.Set("type", "submenu");
        break;
    }

    if (!item->label.empty()) {
      d.Set("label", item->label);
    }
    if (!item->tool_tip.empty()) {
      d.Set("tool_tip", item->tool_tip);
    }
    d.Set("enabled", item->enabled);
    d.Set("checked", item->checked);

    list.Append(std::move(d));
  }
  return list;
}
```

**Step 3: Add to AbpController**

In `chrome/browser/abp/abp_controller.h`, add near `pending_file_choosers_` (line ~942):

```cpp
// Include
#include "chrome/browser/abp/abp_popup_interceptor.h"

// Member (near other owned objects):
std::unique_ptr<AbpPopupInterceptor> popup_interceptor_;

// Public methods:
// Get tab ID for a WebContents (lookup by DevToolsAgentHost ID)
std::string GetTabIdForWebContents(content::WebContents* wc);

// Get the currently active tab ID
std::string GetActiveTabId();

// Emit a popup event (called by AbpPopupInterceptor)
void EmitPopupEvent(const std::string& event_type,
                    base::Value::Dict event_data);

// Get popup interceptor
AbpPopupInterceptor* popup_interceptor() { return popup_interceptor_.get(); }
```

In `chrome/browser/abp/abp_controller.cc`:

- In the constructor (after creating `event_collector_`), create the interceptor:
  ```cpp
  popup_interceptor_ = std::make_unique<AbpPopupInterceptor>(this);
  ```

- Implement `GetTabIdForWebContents` — iterate the TabStripModel, find the WebContents, return its DevToolsAgentHost ID. (ABP already does this lookup in several places — find the pattern with `DevToolsAgentHost::GetOrCreateFor`.)

- Implement `GetActiveTabId` — get the active WebContents from TabStripModel, return its tab ID.

- Implement `EmitPopupEvent` — call `event_collector_->AddEvent(event_type, std::move(event_data))`.

- In the tab-close cleanup code (where `event_observer_->DetachTab(tab_id)` is called), add:
  ```cpp
  popup_interceptor_->CleanupForTab(tab_id);
  ```

**Step 4: Register interceptor with WebContents**

When ABP observes a new tab (in `TabStripModelObserver::OnTabStripModelChanged` or equivalent), call:

```cpp
web_contents->SetPopupInterceptor(popup_interceptor_.get());
```

When a tab is closed, the WebContents is destroyed and the raw pointer becomes stale — this is fine since `CleanupForTab` removes pending state, and the PopupInterceptor outlives individual WebContents.

**Step 5: Update BUILD.gn**

Add to `chrome/browser/abp/BUILD.gn` sources:

```gn
"abp_popup_interceptor.cc",
"abp_popup_interceptor.h",
```

**Step 6: Build and verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles. Interceptor is registered but no REST/MCP tools exist yet to respond.

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_popup_interceptor.h \
       chrome/browser/abp/abp_popup_interceptor.cc \
       chrome/browser/abp/abp_controller.h \
       chrome/browser/abp/abp_controller.cc \
       chrome/browser/abp/BUILD.gn
git commit -m "feat(abp): add AbpPopupInterceptor for select and color picker interception"
```

---

### Task 3: Add REST endpoints for select popup and color picker

Wire up the HTTP endpoints that agents use to respond to intercepted popups.

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc` (routing section ~line 2068, new handler methods)
- Modify: `chrome/browser/abp/abp_controller.h` (new method declarations)

**Step 1: Add URL routing for select popup**

In `abp_controller.cc`, after the `file-chooser` route (line ~2079), add:

```cpp
// Route: /api/v1/select/{id}
if (resource == "select") {
  if (segments.size() == 4) {
    const std::string& popup_id = segments[3];
    if (method == "POST") {
      HandleSelectPopup(popup_id, params, std::move(callback));
    } else {
      SendError(405, "Method not allowed", std::move(callback));
    }
    return;
  }
}

// Route: /api/v1/color-picker/{id}
if (resource == "color-picker") {
  if (segments.size() == 4) {
    const std::string& popup_id = segments[3];
    if (method == "POST") {
      HandleColorPicker(popup_id, params, std::move(callback));
    } else {
      SendError(405, "Method not allowed", std::move(callback));
    }
    return;
  }
}

// Route: /api/v1/popups (list all pending)
if (resource == "popups") {
  if (segments.size() == 3 && method == "GET") {
    HandleListPopups(std::move(callback));
    return;
  }
}
```

**Step 2: Implement HandleSelectPopup**

In `abp_controller.cc`:

```cpp
void AbpController::HandleSelectPopup(const std::string& popup_id,
                                       const base::Value::Dict& params,
                                       ResponseCallback callback) {
  if (!popup_interceptor_) {
    SendError(500, "Popup interceptor not initialized", std::move(callback));
    return;
  }

  // Check if cancel requested
  if (params.FindBool("cancel").value_or(false)) {
    if (popup_interceptor_->CancelSelectPopup(popup_id)) {
      base::Value::Dict result;
      result.Set("success", true);
      result.Set("cancelled", true);
      SendJson(200, std::move(result), std::move(callback));
    } else {
      SendError(404, "Select popup not found: " + popup_id,
                std::move(callback));
    }
    return;
  }

  // Get indices
  const base::Value::List* indices_list = params.FindList("indices");
  if (!indices_list || indices_list->empty()) {
    SendError(400, "Missing or empty 'indices' array", std::move(callback));
    return;
  }

  std::vector<int32_t> indices;
  for (const auto& v : *indices_list) {
    if (!v.is_int()) {
      SendError(400, "indices must contain integers", std::move(callback));
      return;
    }
    indices.push_back(v.GetInt());
  }

  // Validate indices against menu items
  auto popup_info = popup_interceptor_->GetPendingSelectPopup(popup_id);
  if (popup_info.empty()) {
    SendError(404, "Select popup not found: " + popup_id,
              std::move(callback));
    return;
  }

  // Send the selection (this calls DidAcceptIndices on the Mojo remote)
  if (!popup_interceptor_->RespondToSelectPopup(popup_id, indices)) {
    SendError(500, "Failed to respond to select popup", std::move(callback));
    return;
  }

  // Return success with the tab_id for potential follow-up actions
  base::Value::Dict result;
  result.Set("success", true);
  result.Set("tab_id", *popup_info.FindString("tab_id"));
  SendJson(200, std::move(result), std::move(callback));
}
```

**Step 3: Implement HandleColorPicker**

```cpp
void AbpController::HandleColorPicker(const std::string& popup_id,
                                       const base::Value::Dict& params,
                                       ResponseCallback callback) {
  if (!popup_interceptor_) {
    SendError(500, "Popup interceptor not initialized", std::move(callback));
    return;
  }

  if (params.FindBool("cancel").value_or(false)) {
    if (popup_interceptor_->CancelColorPicker(popup_id)) {
      base::Value::Dict result;
      result.Set("success", true);
      result.Set("cancelled", true);
      SendJson(200, std::move(result), std::move(callback));
    } else {
      SendError(404, "Color picker not found: " + popup_id,
                std::move(callback));
    }
    return;
  }

  const std::string* color_str = params.FindString("color");
  if (!color_str) {
    SendError(400, "Missing 'color' parameter", std::move(callback));
    return;
  }

  // Parse hex color (#rrggbb)
  if (color_str->size() != 7 || (*color_str)[0] != '#') {
    SendError(400, "Invalid color format. Expected '#rrggbb'",
              std::move(callback));
    return;
  }

  unsigned int r, g, b;
  if (sscanf(color_str->c_str() + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
    SendError(400, "Invalid hex color value", std::move(callback));
    return;
  }

  SkColor color = SkColorSetRGB(r, g, b);

  auto picker_info = popup_interceptor_->GetPendingColorPicker(popup_id);
  if (picker_info.empty()) {
    SendError(404, "Color picker not found: " + popup_id,
              std::move(callback));
    return;
  }

  if (!popup_interceptor_->RespondToColorPicker(popup_id, color)) {
    SendError(500, "Failed to respond to color picker", std::move(callback));
    return;
  }

  base::Value::Dict result;
  result.Set("success", true);
  result.Set("tab_id", *picker_info.FindString("tab_id"));
  SendJson(200, std::move(result), std::move(callback));
}
```

**Step 4: Implement HandleListPopups**

```cpp
void AbpController::HandleListPopups(ResponseCallback callback) {
  if (!popup_interceptor_) {
    SendJson(200, base::Value::Dict(), std::move(callback));
    return;
  }

  base::Value::Dict result;
  result.Set("popups", popup_interceptor_->GetAllPendingPopups());
  SendJson(200, std::move(result), std::move(callback));
}
```

**Step 5: Declare in header**

In `abp_controller.h` private section (near `HandleFileChooser`):

```cpp
void HandleSelectPopup(const std::string& popup_id,
                       const base::Value::Dict& params,
                       ResponseCallback callback);
void HandleColorPicker(const std::string& popup_id,
                       const base::Value::Dict& params,
                       ResponseCallback callback);
void HandleListPopups(ResponseCallback callback);
```

**Step 6: Build and verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles. REST endpoints are reachable.

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add REST endpoints for select popup and color picker response"
```

---

### Task 4: Add MCP tools (browser_select_picker, browser_color_picker)

Wire up the MCP tool definitions and handlers.

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` (tool definitions + handlers)
- Modify: `chrome/browser/abp/abp_mcp_handler.h` (new method declarations)
- Modify: `chrome/browser/abp/abp_tool_builder.h` (add `OptionalIntegerArray`)
- Modify: `chrome/browser/abp/abp_tool_builder.cc` (implement `OptionalIntegerArray`)

**Step 1: Add OptionalIntegerArray to ToolBuilder**

In `abp_tool_builder.h`, add after `OptionalStringArray`:

```cpp
ToolBuilder& OptionalIntegerArray(const std::string& name,
                                   const std::string& description);
```

In `abp_tool_builder.cc`, implement:

```cpp
ToolBuilder& ToolBuilder::OptionalIntegerArray(const std::string& name,
                                                const std::string& description) {
  base::Value::Dict items;
  items.Set("type", "integer");
  AddArrayProperty(name, description, std::move(items), /*required=*/false);
  return *this;
}
```

**Step 2: Add tool definitions**

In `abp_mcp_handler.cc` `GetToolDefinitions()`, after the `browser_files` tool definition, add:

```cpp
// browser_select_picker
tools.Append(
    ToolBuilder("browser_select_picker")
        .Description(
            "Respond to a pending select popup by choosing option(s). "
            "Use 'cancel' to dismiss without selecting.")
        .RequiredString("popup_id",
                        "The select popup ID from the select_open event")
        .OptionalIntegerArray("indices",
                              "Index(es) of option(s) to select")
        .OptionalBoolean("cancel",
                         "Dismiss the popup without selecting")
        .Build());

// browser_color_picker
tools.Append(
    ToolBuilder("browser_color_picker")
        .Description(
            "Respond to a pending color picker by choosing a color. "
            "Use 'cancel' to dismiss.")
        .RequiredString("popup_id",
                        "The color picker ID from the color_picker_open event")
        .OptionalString("color",
                        "Hex color value (e.g. '#ff5500')")
        .OptionalBoolean("cancel",
                         "Dismiss the picker without selecting")
        .Build());
```

**Step 3: Add tool dispatch**

In `abp_mcp_handler.cc`, in the tool name dispatch section (find the `browser_files` dispatch), add:

```cpp
else if (*name == "browser_select_picker") {
  CallBrowserSelectPicker(*args, std::move(request_id), std::move(callback));
}
else if (*name == "browser_color_picker") {
  CallBrowserColorPicker(*args, std::move(request_id), std::move(callback));
}
```

**Step 4: Implement MCP handlers**

In `abp_mcp_handler.cc`:

```cpp
void AbpMcpHandler::CallBrowserSelectPicker(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* popup_id = args.FindString("popup_id");
  if (!popup_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing popup_id", std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("popup_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/select/" + *popup_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserColorPicker(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* popup_id = args.FindString("popup_id");
  if (!popup_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing popup_id", std::move(callback));
    return;
  }

  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("popup_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/color-picker/" + *popup_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 5: Declare in header**

In `abp_mcp_handler.h`:

```cpp
void CallBrowserSelectPicker(const base::Value::Dict& args,
                              base::Value request_id,
                              ResponseWithHeadersCallback callback);
void CallBrowserColorPicker(const base::Value::Dict& args,
                             base::Value request_id,
                             ResponseWithHeadersCallback callback);
```

**Step 6: Build and verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles. MCP tools appear in `tools/list`.

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h \
       chrome/browser/abp/abp_mcp_handler.cc \
       chrome/browser/abp/abp_tool_builder.h \
       chrome/browser/abp/abp_tool_builder.cc
git commit -m "feat(abp): add browser_select_picker and browser_color_picker MCP tools"
```

---

### Task 5: Manual integration test with a real select dropdown

Verify the full flow works end-to-end with a real website.

**Files:**
- No code changes — testing only

**Step 1: Launch ABP**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Navigate to a page with a select dropdown**

```bash
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"data:text/html,<select id=\"test\"><option value=\"a\">Apple</option><option value=\"b\">Banana</option><option value=\"c\" selected>Cherry</option></select>"}'
```

**Step 3: Click on the select element to trigger popup**

Use the screenshot to find coordinates, then click:

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":50,"y":15}'
```

**Step 4: Verify popup was intercepted**

```bash
curl http://localhost:8222/api/v1/popups
```

Expected: Response contains a `select_open` entry with 3 items (Apple, Banana, Cherry), `selected_index: 2`.

**Step 5: Respond with a selection**

```bash
curl -X POST http://localhost:8222/api/v1/select/sp_1 \
  -H "Content-Type: application/json" \
  -d '{"indices":[1]}'
```

Expected: `{"success": true, "tab_id": "..."}`

**Step 6: Verify the selection took effect**

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/execute \
  -H "Content-Type: application/json" \
  -d '{"script":"document.getElementById(\"test\").value"}'
```

Expected: `"b"` (Banana was selected)

**Step 7: Test cancel flow**

Click the select again, then cancel:

```bash
curl -X POST http://localhost:8222/api/v1/select/sp_2 \
  -H "Content-Type: application/json" \
  -d '{"cancel":true}'
```

Verify value unchanged (still "b").

**Step 8: Test on reduceimages.com**

Navigate to `https://www.reduceimages.com/` and test the actual dropdown that originally caused the issue. Verify the select popup is intercepted and agent can make a selection.

---

### Task 6: Update API documentation and MCP skill

Update docs to include the new endpoints and tools.

**Files:**
- Modify: `plans/API.md` (add new endpoints to table)
- Modify: `tools/abp-claude-skill/abp-browser.md` (add tool descriptions for Claude)
- Modify: `CLAUDE.md` (update endpoint table)

**Step 1: Add endpoints to API.md**

Add to the endpoint table:

```markdown
| **Popups** | | |
| GET | `/api/v1/popups` | List pending native popups |
| POST | `/api/v1/select/{id}` | Respond to select popup |
| POST | `/api/v1/color-picker/{id}` | Respond to color picker |
```

**Step 2: Add tools to MCP skill**

Add `browser_select_picker` and `browser_color_picker` tool descriptions to the Claude skill file.

**Step 3: Update CLAUDE.md endpoint table**

Add the new endpoints to the API Reference table.

**Step 4: Commit**

```bash
git add plans/API.md tools/abp-claude-skill/abp-browser.md CLAUDE.md
git commit -m "docs: add native popup interception endpoints and MCP tools"
```
