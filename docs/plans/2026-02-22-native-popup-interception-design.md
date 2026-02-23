# Native Popup Interception Design

## Problem

When ABP automates pages with `<select>` dropdowns, Chromium shows native OS menus (NSMenu on macOS) that agents cannot interact with. The automation gets stuck because ABP has no tools to see or respond to native popups.

## Scope

- **In scope**: `<select>` dropdowns, color pickers (`<input type="color">`)
- **Out of scope**: Date/time pickers (desktop uses DOM popups), context menus (not needed for agent navigation)

## Architecture

Follow ABP's established **intercept-and-respond** pattern (proven with file choosers):

1. **Intercept** the Mojo call at `RenderFrameHostImpl` before platform UI is shown
2. **Capture** the popup's data (menu items, current value, etc.)
3. **Suppress** the native UI (don't delegate to platform helper)
4. **Expose** the popup via REST API + MCP events
5. **Wait** for the agent to respond with a selection
6. **Complete** the interaction by calling the renderer's Mojo callback

### Interception Mechanism: Delegate Pattern

Define a `PopupMenuInterceptor` interface in `content/public/browser/`. ABP registers its implementation during startup. `RenderFrameHostImpl::ShowPopupMenu()` checks for a registered interceptor before delegating to platform UI.

This keeps ABP code in `chrome/browser/abp/` with only a minimal interface check in core Chromium. Cleaner rebasing, follows Chromium's delegate patterns (WebContentsDelegate, WebContentsObserver).

### Select Popup Flow

```
Renderer: ExternalPopupMenu::ShowInternal()
  -> Mojo IPC ->
Browser: RenderFrameHostImpl::ShowPopupMenu()
  -> Check PopupMenuInterceptor
    -> ABP stores PopupMenuClient remote + menu items
    -> Emits select_open event
    -> Suppresses native UI
    -> Waits for agent response via REST/MCP
    -> Calls PopupMenuClient::DidAcceptIndices() or DidCancel()
```

### Color Picker Flow

Similar pattern — intercept `WebContentsImpl::OpenColorChooser()` before platform UI, store `ColorChooserClient` remote, emit event, wait for agent, call `DidChooseColor()`.

## Event Types

### `select_open`

Emitted when a `<select>` popup is intercepted:

```json
{
  "type": "select_open",
  "id": "sp_1",
  "tab_id": "ABC123",
  "bounds": {"x": 150, "y": 300, "width": 200, "height": 24},
  "allow_multiple_selection": false,
  "selected_index": 2,
  "items": [
    {"index": 0, "label": "Small (64x64)", "type": "option", "enabled": true, "checked": false},
    {"index": 1, "label": "Medium (128x128)", "type": "option", "enabled": true, "checked": false},
    {"index": 2, "label": "Large (256x256)", "type": "option", "enabled": true, "checked": true},
    {"index": 3, "type": "separator"},
    {"index": 4, "label": "Custom...", "type": "option", "enabled": false, "checked": false},
    {"index": 5, "label": "Size Options", "type": "group", "sub_items": ["..."]}
  ]
}
```

### `color_picker_open`

Emitted when a color picker is intercepted:

```json
{
  "type": "color_picker_open",
  "id": "cp_1",
  "tab_id": "ABC123",
  "current_color": "#ff5500",
  "suggestions": [
    {"color": "#ff0000", "label": "Red"},
    {"color": "#00ff00", "label": "Green"}
  ]
}
```

## MCP Tools

### `browser_select_picker`

Standalone tool (not batchable in `browser_action`). Responds to a pending select popup.

```json
{
  "name": "browser_select_picker",
  "description": "Respond to a pending select popup by choosing option(s). Use 'cancel' to dismiss without selecting.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "popup_id": {
        "type": "string",
        "description": "The select popup ID from the select_open event"
      },
      "indices": {
        "type": "array",
        "items": {"type": "integer"},
        "description": "Index(es) of option(s) to select"
      },
      "cancel": {
        "type": "boolean",
        "description": "Dismiss the popup without selecting"
      }
    },
    "required": ["popup_id"]
  }
}
```

**REST**: `POST /api/v1/select/{popup_id}`

### `browser_color_picker`

Standalone tool. Responds to a pending color picker.

```json
{
  "name": "browser_color_picker",
  "description": "Respond to a pending color picker by choosing a color. Use 'cancel' to dismiss.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "popup_id": {
        "type": "string",
        "description": "The color picker ID from the color_picker_open event"
      },
      "color": {
        "type": "string",
        "description": "Hex color value (e.g. '#ff5500')"
      },
      "cancel": {
        "type": "boolean",
        "description": "Dismiss the picker without selecting"
      }
    },
    "required": ["popup_id"]
  }
}
```

**REST**: `POST /api/v1/color-picker/{popup_id}`

## Discovery

Agents discover pending popups via:

1. **Events on action result**: When a `browser_action` click triggers a popup, the action response includes the event in `events[]`
2. **Direct query**: `GET /api/v1/popups` lists all pending popups (analogous to `GET /api/v1/tabs/{id}/dialog`)

## C++ Components

### New Interface: `PopupMenuInterceptor`

Location: `content/public/browser/popup_menu_interceptor.h`

```cpp
class PopupMenuInterceptor {
 public:
  virtual bool OnSelectPopupRequested(
      RenderFrameHost* rfh,
      mojo::PendingRemote<blink::mojom::PopupMenuClient> popup_client,
      const gfx::Rect& bounds,
      int32_t selected_item,
      std::vector<blink::mojom::MenuItemPtr> menu_items,
      bool allow_multiple) = 0;
};
```

### Storage in AbpController

```cpp
struct PendingSelectPopup {
  std::string tab_id;
  mojo::Remote<blink::mojom::PopupMenuClient> client;
  std::vector<blink::mojom::MenuItemPtr> items;
  int32_t selected_index;
  bool allow_multiple;
  gfx::Rect bounds;
};
std::map<std::string, PendingSelectPopup> pending_select_popups_;

struct PendingColorPicker {
  std::string tab_id;
  mojo::Remote<blink::mojom::ColorChooserClient> client;
  uint32_t current_color;
  std::vector<blink::mojom::ColorSuggestionPtr> suggestions;
};
std::map<std::string, PendingColorPicker> pending_color_pickers_;
```

### Core Chromium Change

Minimal change in `RenderFrameHostImpl::ShowPopupMenu()`:

```cpp
void RenderFrameHostImpl::ShowPopupMenu(...) {
  auto* interceptor = GetPopupMenuInterceptor();
  if (interceptor && interceptor->OnSelectPopupRequested(
          this, std::move(popup_client), bounds, selected_item,
          std::move(menu_items), allow_multiple_selection)) {
    return;  // Intercepted, skip native UI
  }
  // ... normal Chromium flow
}
```

## Error Handling

- **60s timeout**: Auto-cancel popup if agent doesn't respond, preventing renderer hang
- **Tab closure**: Mojo remote disconnects naturally; clean up pending map
- **Renderer crash/navigation**: Mojo disconnect handler cleans up
- **Invalid index**: Return error, popup stays pending (agent can retry)
- **Disabled/separator index**: Return error
- **Expired popup ID**: Return 404
- **One popup per tab**: Chromium enforces this; different tabs can have concurrent popups

## Action Context Integration

`browser_select_picker` and `browser_color_picker` responses run through `AbpActionContext`:
- Pause → send selection via Mojo → wait for page settle → screenshot
- Events captured during the action (network, DOM mutations from `change` handlers) included in response
