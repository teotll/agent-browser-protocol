# ABP REST API Reference

The Agent Browser Protocol (ABP) exposes a REST API on `localhost:8222` for AI agent browser control. All endpoints use JSON request/response bodies and operate directly at the browser engine level for low latency and high capability.

---

## Quick Start

### 1. Start ABP

**macOS:**
```bash
./ABP.app/Contents/MacOS/ABP
```

**Linux:**
```bash
./abp
```

### 2. Verify the browser is ready

```bash
curl http://localhost:8222/api/v1/browser/status
```

### 3. Full walkthrough

```bash
# List open tabs
curl http://localhost:8222/api/v1/tabs

# Create a new tab
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Navigate an existing tab (replace {tab_id} with actual ID from above)
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Click at coordinates
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":200}'

# Type text
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/type \
  -H "Content-Type: application/json" \
  -d '{"text":"hello world"}'

# Take a screenshot with interactive element markup
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/screenshot \
  -H "Content-Type: application/json" \
  -d '{"screenshot":{"markup":"interactive","format":"webp"}}'

# Get page text
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/text \
  -H "Content-Type: application/json" \
  -d '{}'

# Execute JavaScript (note: parameter is "script", not "expression")
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/execute \
  -H "Content-Type: application/json" \
  -d '{"script":"document.title"}'

# Binary screenshot (returns image/webp directly)
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=interactive -o screenshot.webp

# Close the tab
curl -X DELETE http://localhost:8222/api/v1/tabs/{tab_id}
```

---

## Standard Response Envelope

Every action endpoint returns a standard response envelope with the following fields:

```json
{
  "result": { },
  "screenshot_before": {
    "data": "<base64-encoded image>",
    "width": 1280,
    "height": 720,
    "virtual_time_ms": 0,
    "format": "webp"
  },
  "screenshot_after": {
    "data": "<base64-encoded image>",
    "width": 1280,
    "height": 720,
    "virtual_time_ms": 0,
    "format": "webp"
  },
  "scroll": {
    "scrollX": 0,
    "scrollY": 150,
    "pageWidth": 1280,
    "pageHeight": 4000,
    "viewportWidth": 1280,
    "viewportHeight": 720
  },
  "events": [
    {
      "type": "navigation",
      "virtual_time_ms": 0,
      "data": { }
    }
  ],
  "timing": {
    "action_started_ms": 1700000000000,
    "action_completed_ms": 1700000000050,
    "wait_completed_ms": 1700000000050,
    "duration_ms": 50
  },
  "cursor": {
    "x": 100,
    "y": 200,
    "cursor_type": "pointer"
  },
  "virtual_time": {
    "paused": false,
    "base_ticks_ms": 0
  }
}
```

| Field | Description |
|-------|-------------|
| `result` | Action-specific result data (varies by endpoint) |
| `screenshot_before` | Base64-encoded WebP screenshot taken before the action |
| `screenshot_after` | Base64-encoded WebP screenshot taken after the action |
| `scroll` | Current scroll position and page/viewport dimensions |
| `events` | Array of events that occurred during the action |
| `timing` | Timestamps and duration of the action lifecycle |
| `cursor` | Current cursor position and CSS cursor type |
| `virtual_time` | Virtual time state (present when execution control is enabled) |

---

## Screenshot Options

Screenshots can be customized via the `screenshot` field in action request bodies or via query parameters on the GET endpoint.

### Markup

The `markup` field controls which element overlays are drawn on the screenshot. It accepts either a preset string or an array of overlay types:

**Preset:**
- `"interactive"` — equivalent to `["clickable", "typeable", "scrollable"]`

**Individual overlay types (array):**

| Type | Description |
|------|-------------|
| `clickable` | Highlights clickable elements (links, buttons, etc.) |
| `typeable` | Highlights text input fields |
| `scrollable` | Highlights scrollable containers |
| `grid` | Draws a coordinate grid overlay |
| `selected` | Highlights the currently selected/focused element |

**Example:**
```json
{
  "screenshot": {
    "markup": ["clickable", "typeable", "grid"]
  }
}
```

### Other Options

| Field | Type | Description |
|-------|------|-------------|
| `disable_markup` | boolean | Set to `true` to disable all markup overlays |
| `format` | string | Image format: `"png"`, `"webp"` (default), or `"jpeg"` |

---

## Event Types

Events are collected during action execution and returned in the `events` array of the response envelope.

| Type | Description |
|------|-------------|
| `navigation` | Page navigation occurred (load, redirect, etc.) |
| `dialog` | A JavaScript dialog appeared (alert, confirm, prompt, beforeunload) |
| `file_chooser` | A file picker dialog was opened |
| `popup` | A popup window was opened |
| `tab_closed` | A tab was closed |
| `scroll` | The page was scrolled |
| `download_started` | A file download began |
| `download_completed` | A file download finished |
| `file_selected` | Files were chosen or saved in a file chooser |
| `file_chooser_cancelled` | A file chooser was dismissed without selection |
| `select_open` | A native `<select>` dropdown opened (includes option list) |
| `permission_requested` | A permission prompt appeared (geolocation, camera, etc.) |

Each event includes:
- `type` — one of the types above
- `virtual_time_ms` — virtual time when the event occurred
- `data` — event-specific payload

---

## Endpoint Reference

All endpoints are prefixed with `/api/v1`.

### Browser

| Method | Path | Description |
|--------|------|-------------|
| GET | `/browser/status` | Get browser readiness status |
| GET | `/browser/session-data` | Get session data file paths |
| POST | `/browser/shutdown` | Graceful browser shutdown |

### Tabs

| Method | Path | Description |
|--------|------|-------------|
| GET | `/tabs` | List all open tabs |
| GET | `/tabs/{id}` | Get tab details |
| POST | `/tabs` | Create a new tab |
| DELETE | `/tabs/{id}` | Close a tab |
| POST | `/tabs/{id}/activate` | Switch to a tab |
| POST | `/tabs/{id}/stop` | Stop page loading |

### Navigation

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/navigate` | Navigate to a URL |
| POST | `/tabs/{id}/back` | Go back in history |
| POST | `/tabs/{id}/forward` | Go forward in history |
| POST | `/tabs/{id}/reload` | Reload the page |

### Mouse

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/click` | Click at coordinates |
| POST | `/tabs/{id}/move` | Move mouse to coordinates |
| POST | `/tabs/{id}/scroll` | Scroll via mouse wheel at coordinates |
| POST | `/tabs/{id}/drag` | Drag from start to end coordinates |

### Keyboard

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/type` | Type text string |
| POST | `/tabs/{id}/keyboard/press` | Press a key combination |
| POST | `/tabs/{id}/keyboard/down` | Key down event |
| POST | `/tabs/{id}/keyboard/up` | Key up event |

### Input Helpers

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/slider` | Move a slider to a target value |
| POST | `/tabs/{id}/clear-text` | Clear an input field (click, select all, backspace) |

### Screenshots

| Method | Path | Description |
|--------|------|-------------|
| GET | `/tabs/{id}/screenshot` | Binary screenshot (returns image directly) |
| POST | `/tabs/{id}/screenshot` | Screenshot via action envelope |

### Content

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/execute` | Execute JavaScript (`script` parameter) |
| POST | `/tabs/{id}/text` | Get page text (or text from a CSS selector) |

### Wait

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/wait` | Wait for a specified duration |

### Dialogs

| Method | Path | Description |
|--------|------|-------------|
| GET | `/tabs/{id}/dialog` | Get pending dialog info |
| POST | `/tabs/{id}/dialog/accept` | Accept the pending dialog |
| POST | `/tabs/{id}/dialog/dismiss` | Dismiss the pending dialog |

### Execution Control

| Method | Path | Description |
|--------|------|-------------|
| GET | `/tabs/{id}/execution` | Get execution state (paused/running) |
| POST | `/tabs/{id}/execution` | Set execution state (pause/resume with virtual time) |

### Downloads

| Method | Path | Description |
|--------|------|-------------|
| GET | `/downloads` | List all downloads |
| GET | `/downloads/{id}` | Get download status |
| POST | `/downloads/{id}/cancel` | Cancel a download |
| GET | `/downloads/{id}/content` | Get download content as base64 |

### File Chooser

| Method | Path | Description |
|--------|------|-------------|
| POST | `/file-chooser/{id}` | Provide files to a native file picker dialog |

### Popups

| Method | Path | Description |
|--------|------|-------------|
| POST | `/select/{id}` | Respond to a native `<select>` dropdown |

### Permissions

| Method | Path | Description |
|--------|------|-------------|
| GET | `/permissions` | List pending permission requests |
| POST | `/permissions/{id}/grant` | Grant a permission (geolocation requires lat/lng) |
| POST | `/permissions/{id}/deny` | Deny a permission |

### History

| Method | Path | Description |
|--------|------|-------------|
| GET | `/history/sessions` | List all sessions |
| GET | `/history/sessions/current` | Get the current session |
| GET | `/history/sessions/{id}` | Get a session by ID |
| GET | `/history/sessions/{id}/export` | Export a session |
| GET | `/history/actions` | List actions |
| GET | `/history/actions/{id}` | Get an action by ID |
| GET | `/history/actions/{id}/screenshot` | Get an action's screenshot |
| DELETE | `/history/actions` | Delete actions |
| GET | `/history/events` | List events |
| GET | `/history/events/{id}` | Get an event by ID |
| DELETE | `/history/events` | Delete events |
| DELETE | `/history` | Delete all history |

### Batch

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/batch` | Execute multiple actions in sequence |

---

## Full Specification

For detailed request/response schemas, parameter descriptions, and implementation notes, see [plans/API.md](../plans/API.md).
