# Agent Browser Protocol - API Specification

## Implementation Status

This document specifies the complete ABP REST API. The following table shows current implementation status:

| Category | Implemented | Planned |
|----------|-------------|---------|
| Tab Management | list, create, close, get, activate, stop | pin, mute, duplicate, move |
| Navigation | navigate, back, forward, reload | - |
| Mouse | click, move, scroll | drag, hover, mouse down/up |
| Keyboard | type, press, down, up | shortcut, insert |
| Screenshots | viewport (GET/POST), markup, cursor | full-page, region |
| JavaScript | execute | - |
| Text | get text (full page or selector) | - |
| Dialogs | get, accept, dismiss | - |
| Downloads | list, get, cancel | configure, wait, resume |
| File Chooser | provide files | configure, get pending |
| Browser | status, shutdown | get info |
| Execution Control | get/set state | - |
| History | sessions, actions, events, export | - |
| Network | - | intercept, requests |
| Window | - | bounds, state |
| Cookies | - | get, set, clear |
| Wait | duration wait | navigation, network idle |
| MCP | 30 tools via /mcp endpoint | - |

Features marked "Planned" are documented below but not yet implemented.

---

## Base URL

```
http://localhost:8222/api/v1
```

## Authentication

**Not Yet Implemented**

If `--abp-auth-token` is set, all requests must include:

```
Authorization: Bearer <token>
```

---

## Standard Request Envelope (Actions)

All action endpoints (POST/DELETE that modify state) accept standard parameters:

```json
{
  "action_params": { ... },
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 30000
  },
  "screenshot": {
    "area": "viewport",
    "markup": "interactive",
    "cursor": true
  }
}
```

### Screenshot Options

Control screenshot capture and element markup in the response.

#### Screenshot Area

| Value | Description |
|-------|-------------|
| `none` | No screenshot returned |
| `viewport` | Capture visible viewport (default) |

#### Screenshot Markup

Bounding boxes drawn over elements to help identify interactive targets:

| Value | Description |
|-------|-------------|
| `none` | No markup overlay (default) |
| `interactive` | All interactive elements (clickable + typeable) |
| `clickable` | Buttons, links, and other clickable elements |
| `typeable` | Text inputs, textareas, contenteditable elements |
| `inputs` | All form inputs (text, checkbox, radio, select, etc.) |

**Markup appearance:**
- Each marked element gets a colored bounding box
- Box colors indicate element type (e.g., blue for links, green for buttons, orange for inputs)
- Element index labels are drawn for reference
- Boxes are semi-transparent to not obscure content

#### Screenshot Cursor

Control virtual cursor visibility in screenshots. The virtual cursor is automatically positioned by input actions (click, scroll, move) and rendered at the compositor layer.

| Value | Description |
|-------|-------------|
| `true` | Include virtual cursor in screenshot (default) |
| `false` | Hide virtual cursor for screenshot capture |

**Example request with markup and cursor:**
```json
{
  "x": 100,
  "y": 200,
  "wait_until": {"type": "action_complete"},
  "screenshot": {
    "area": "viewport",
    "markup": "interactive",
    "cursor": true
  }
}
```

### Wait Until Types

| Type | Description |
|------|-------------|
| `immediate` | Return immediately after dispatching the action |
| `action_complete` | Wait for engine rendering/navigation lull after action (default) |
| `time` | Wait for a fixed duration specified by `duration_ms` |

### Wait Until Parameters

```json
{
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 30000,
    "duration_ms": 1000
  }
}
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | `action_complete` | Wait condition type |
| `timeout_ms` | number | 30000 | Maximum time to wait before returning |
| `duration_ms` | number | - | Fixed wait for `time` type |

### Action Complete Heuristic

The `action_complete` type uses engine-level signals to detect completion:

1. **Rendering quiescence**: No pending paint/layout operations
2. **Navigation settled**: No pending navigations or redirects
3. **Script idle**: No pending JavaScript tasks or promises
4. **Minimum delay**: Configurable minimum wait (default: 100ms) after last activity

---

## Standard Response Envelope (Actions)

All action responses include before and after screenshots, scroll position, and event log:

```json
{
  "result": { ... },
  "screenshot_before": {
    "data": "base64-encoded-image",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999000,
    "format": "webp"
  },
  "screenshot_after": {
    "data": "base64-encoded-image",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999999,
    "format": "webp"
  },
  "scroll": {
    "horizontal_percent": 0,
    "vertical_percent": 25.5,
    "horizontal_px": 0,
    "vertical_px": 1200,
    "page_width": 1920,
    "page_height": 4700,
    "viewport_width": 1920,
    "viewport_height": 1080
  },
  "events": [
    {
      "type": "navigation",
      "virtual_time_ms": 1699999999100,
      "data": { ... }
    }
  ],
  "timing": {
    "action_started_ms": 1699999999000,
    "action_completed_ms": 1699999999050,
    "wait_completed_ms": 1699999999500,
    "duration_ms": 500
  }
}
```

### Screenshot Objects

Both `screenshot_before` and `screenshot_after` share the same format:

| Field | Type | Description |
|-------|------|-------------|
| `data` | string | Base64-encoded image |
| `width` | number | Image width in pixels |
| `height` | number | Image height in pixels |
| `virtual_time_ms` | number | Virtual time when screenshot was captured (ms since epoch) |
| `format` | string | Image format (default `webp`) |

- `screenshot_before` is captured just before the action executes, reflecting the page state the agent sees.
- `screenshot_after` is captured after the action completes and wait conditions are met.
- When `screenshot.markup` is set in the request, both screenshots include element markup overlays.
- In MCP responses, both screenshots are returned as separate image content blocks (before first, then after).

### Scroll Position

The `scroll` object provides current scroll state after wait completion:

| Field | Type | Description |
|-------|------|-------------|
| `horizontal_percent` | number | Horizontal scroll position as percentage (0-100) |
| `vertical_percent` | number | Vertical scroll position as percentage (0-100) |
| `horizontal_px` | number | Horizontal scroll offset in pixels |
| `vertical_px` | number | Vertical scroll offset in pixels |
| `page_width` | number | Total scrollable page width in pixels |
| `page_height` | number | Total scrollable page height in pixels |
| `viewport_width` | number | Visible viewport width in pixels |
| `viewport_height` | number | Visible viewport height in pixels |

### Event Types

Events are captured between action dispatch and wait completion:

#### `navigation`
Tab navigated to a new URL.

```json
{
  "type": "navigation",
  "virtual_time_ms": 1699999999100,
  "data": {
    "tab_id": "tab_abc123",
    "url": "https://example.com/page",
    "navigation_type": "link_click"
  }
}
```

**navigation_type values:** `link_click`, `form_submit`, `redirect`, `back_forward`, `reload`

#### `dialog`
Browser dialog appeared (alert, confirm, prompt).

```json
{
  "type": "dialog",
  "virtual_time_ms": 1699999999200,
  "data": {
    "tab_id": "tab_abc123",
    "dialog_type": "confirm",
    "message": "Are you sure you want to delete?",
    "default_prompt": "",
    "pending": true
  }
}
```

**dialog_type values:** `alert`, `confirm`, `prompt`, `beforeunload`

#### `file_chooser`
Native file picker dialog appeared. Use the `id` to provide files via `POST /file-chooser/{id}`.

```json
{
  "type": "file_chooser",
  "virtual_time_ms": 1699999999300,
  "data": {
    "id": "fc_abc123",
    "tab_id": "tab_abc123",
    "chooser_type": "open",
    "accepts": [{"description": "Images", "extensions": ["jpg", "png"]}],
    "multiple": false,
    "pending": true
  }
}
```

**chooser_type values:** `open`, `open_multiple`, `save`

#### `popup`
New popup window or tab opened.

```json
{
  "type": "popup",
  "virtual_time_ms": 1699999999400,
  "data": {
    "source_tab_id": "tab_abc123",
    "new_tab_id": "tab_xyz789",
    "url": "https://example.com/popup",
    "popup_type": "window"
  }
}
```

**popup_type values:** `window`, `tab`

#### `tab_closed`
Tab was closed.

```json
{
  "type": "tab_closed",
  "virtual_time_ms": 1699999999500,
  "data": {
    "tab_id": "tab_abc123",
    "reason": "script"
  }
}
```

**reason values:** `script`, `user`, `navigation`

#### `scroll`
Page was scrolled.

```json
{
  "type": "scroll",
  "virtual_time_ms": 1699999999550,
  "data": {
    "tab_id": "tab_abc123",
    "delta": {
      "x": 0,
      "y": 300,
      "direction": "down"
    },
    "position": {
      "horizontal_percent": 0,
      "vertical_percent": 45.2,
      "horizontal_px": 0,
      "vertical_px": 2100
    },
    "source": "wheel"
  }
}
```

**delta.direction values:** `up`, `down`, `left`, `right`

**source values:** `wheel`, `keyboard`, `script`, `drag`

#### `download_started`
Download was initiated.

```json
{
  "type": "download_started",
  "virtual_time_ms": 1699999999600,
  "data": {
    "download_id": "dl_123",
    "url": "https://example.com/file.pdf",
    "filename": "file.pdf",
    "mime_type": "application/pdf",
    "total_bytes": 102400
  }
}
```

#### `download_completed`
Download finished successfully.

```json
{
  "type": "download_completed",
  "virtual_time_ms": 1699999999900,
  "data": {
    "download_id": "dl_123",
    "path": "/downloads/file.pdf",
    "filename": "file.pdf",
    "bytes_received": 102400,
    "mime_type": "application/pdf"
  }
}
```

#### `file_selected`
Files were selected in a file chooser dialog.

```json
{
  "type": "file_selected",
  "virtual_time_ms": 1699999999700,
  "data": {
    "id": "fc_abc123",
    "tab_id": "tab_abc123",
    "chooser_type": "open",
    "files": ["/path/to/document.pdf", "/path/to/image.png"]
  }
}
```

For save dialogs:
```json
{
  "type": "file_selected",
  "virtual_time_ms": 1699999999700,
  "data": {
    "id": "fc_abc123",
    "tab_id": "tab_abc123",
    "chooser_type": "save",
    "path": "/path/to/output.pdf"
  }
}
```

#### `file_chooser_cancelled`
File chooser was dismissed without selection.

```json
{
  "type": "file_chooser_cancelled",
  "virtual_time_ms": 1699999999800,
  "data": {
    "id": "fc_abc123",
    "tab_id": "tab_abc123",
    "chooser_type": "open"
  }
}
```

### Screenshot Configuration

Control screenshot capture via the `screenshot` object in the request body:

```json
{
  "screenshot": {
    "area": "viewport",
    "markup": "interactive",
    "cursor": true
  }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `area` | string | `viewport` | Capture area: `none`, `viewport` |
| `markup` | string | `none` | Element markup: `none`, `interactive`, `clickable`, `typeable`, `inputs` |
| `cursor` | boolean | `true` | Include virtual cursor in screenshot |

**Format:** All screenshots are returned as WebP at quality 80. This is not configurable to ensure consistent bandwidth usage and simplify caching.

---

## Query Response Format (GET endpoints)

GET endpoints return data directly without the action envelope (no screenshot/events):

```json
{ ... }
```

For tab listing, the response is a bare JSON array. Other list endpoints (e.g., downloads) wrap results in an object.

---

## Error Response Format

```json
{
  "error": "Human readable error message"
}
```

HTTP status codes indicate error type:
- `400` - Bad request (invalid parameters)
- `404` - Resource not found (tab, download, etc.)
- `500` - Internal server error

---

## Browser Management

### Get Browser Status

```
GET /browser/status
```

Returns initialization status. Poll this endpoint to wait for ABP to be ready after browser launch.

**Response (initializing):**
```json
{
  "success": true,
  "data": {
    "ready": false,
    "state": "initializing",
    "components": {
      "http_server": true,
      "browser_window": false,
      "devtools": false
    },
    "message": "Waiting for browser window"
  }
}
```

**Response (ready):**
```json
{
  "success": true,
  "data": {
    "ready": true,
    "state": "ready",
    "components": {
      "http_server": true,
      "browser_window": true,
      "devtools": true
    }
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `success` | boolean | Always `true` for this endpoint |
| `data.ready` | boolean | `true` when ABP is fully initialized and ready to accept commands |
| `data.state` | string | Current state: `initializing`, `ready` |
| `data.components.http_server` | boolean | HTTP server is listening |
| `data.components.browser_window` | boolean | Browser window is created and active |
| `data.components.devtools` | boolean | DevTools connection is established |
| `data.message` | string | Human-readable status message (present when not ready) |

**Polling example:**
```bash
# Wait for browser to be ready (bash)
while true; do
  response=$(curl -s http://localhost:8222/api/v1/browser/status)
  ready=$(echo "$response" | jq -r '.data.ready')
  if [ "$ready" = "true" ]; then
    echo "Browser ready"
    break
  fi
  sleep 0.5
done
```

### Shutdown Browser

```
POST /browser/shutdown
```

Gracefully shuts down the browser.

**Request:**
```json
{
  "timeout_ms": 5000
}
```

---

## Tab Management

### List All Tabs

```
GET /tabs
```

Returns all open tabs as an array.

**Response:**
```json
[
  {
    "id": "tab_abc123",
    "url": "https://example.com",
    "title": "Example Domain",
    "active": true
  }
]
```

### Get Tab Info

```
GET /tabs/{tab_id}
```

Returns detailed information about a specific tab.

**Response:**
```json
{
  "id": "tab_abc123",
  "url": "https://example.com",
  "title": "Example Domain",
  "loading": false
}
```

### Create New Tab

```
POST /tabs
```

Creates a new tab.

**Request:**
```json
{
  "url": "https://example.com",
  "active": true,
  "index": 0
}
```

All fields optional. Defaults to blank tab at end, made active.

**Response (201 Created):**
```json
{
  "id": "tab_xyz789",
  "url": "about:blank"
}
```

### Close Tab

```
DELETE /tabs/{tab_id}
```

Closes the specified tab.

**Response:**
```json
{}
```

### Activate Tab (Switch To)

```
POST /tabs/{tab_id}/activate
```

Switches to the specified tab.

**Response:**
```json
{
  "status": "activated",
  "tab_id": "tab_xyz789",
  "index": 0
}
```

---

## Navigation

### Navigate to URL

```
POST /tabs/{tab_id}/navigate
```

**Request:**
```json
{
  "url": "https://example.com",
  "referrer": "https://google.com",
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 30000
  }
}
```

**Response:**
```json
{
  "result": {
    "status": "navigated",
    "url": "https://example.com"
  },
  "screenshot_before": {
    "data": "UklGRlYAAABXRUJQVlA4I...",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999000,
    "format": "webp"
  },
  "screenshot_after": {
    "data": "UklGRlYAAABXRUJQVlA4I...",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999500,
    "format": "webp"
  },
  "scroll": {
    "horizontal_percent": 0,
    "vertical_percent": 0,
    "horizontal_px": 0,
    "vertical_px": 0,
    "page_width": 1920,
    "page_height": 1080,
    "viewport_width": 1920,
    "viewport_height": 1080
  },
  "events": [
    {
      "type": "navigation",
      "virtual_time_ms": 1699999999100,
      "data": {
        "tab_id": "tab_abc123",
        "url": "https://example.com"
      }
    }
  ],
  "timing": {
    "action_started_ms": 1699999999000,
    "action_completed_ms": 1699999999100,
    "wait_completed_ms": 1699999999500,
    "duration_ms": 500
  }
}
```

### Go Back

```
POST /tabs/{tab_id}/back
```

**Request:**
```json
{
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 30000
  }
}
```

Returns 400 if there is no back history.

### Go Forward

```
POST /tabs/{tab_id}/forward
```

**Request:**
```json
{
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 30000
  }
}
```

Returns 400 if there is no forward history.

### Reload

```
POST /tabs/{tab_id}/reload
```

Reloads the page. Always uses normal reload (not cache-bypassing).

**Request:**
```json
{
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 30000
  }
}
```

### Stop Loading

```
POST /tabs/{tab_id}/stop
```

**Response:**
```json
{
  "status": "stopped",
  "tab_id": "tab_abc123"
}
```

---

## Mouse Actions

### Click

```
POST /tabs/{tab_id}/click
```

Performs a mouse click at the specified coordinates.

**Request:**
```json
{
  "x": 100,
  "y": 200,
  "button": "left",
  "click_count": 1,
  "modifiers": [],
  "wait_until": {
    "type": "action_complete",
    "timeout_ms": 5000
  }
}
```

**button options:** `"left"`, `"right"`, `"middle"`

**modifiers options:** `"Shift"`, `"Control"`, `"Alt"`, `"Meta"` (also accepts left/right variants like `"ShiftLeft"`, `"ControlRight"`)

**click_count:** 1 for single click, 2 for double click, 3 for triple click

**Response (example showing dialog triggered by click):**
```json
{
  "result": {
    "status": "clicked"
  },
  "screenshot_before": {
    "data": "UklGRlYAAABXRUJQVlA4I...",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999000,
    "format": "webp"
  },
  "screenshot_after": {
    "data": "UklGRlYAAABXRUJQVlA4I...",
    "width": 1920,
    "height": 1080,
    "virtual_time_ms": 1699999999500,
    "format": "webp"
  },
  "scroll": {
    "horizontal_percent": 0,
    "vertical_percent": 25.5,
    "horizontal_px": 0,
    "vertical_px": 1200,
    "page_width": 1920,
    "page_height": 4700,
    "viewport_width": 1920,
    "viewport_height": 1080
  },
  "events": [
    {
      "type": "dialog",
      "virtual_time_ms": 1699999999200,
      "data": {
        "tab_id": "tab_abc123",
        "dialog_type": "confirm",
        "message": "Delete this item?",
        "pending": true
      }
    }
  ],
  "timing": {
    "action_started_ms": 1699999999000,
    "action_completed_ms": 1699999999050,
    "wait_completed_ms": 1699999999500,
    "duration_ms": 500
  }
}
```

### Mouse Move

```
POST /tabs/{tab_id}/move
```

Moves mouse to coordinates (single move, no interpolation).

**Request:**
```json
{
  "x": 100,
  "y": 200
}
```

### Scroll (Wheel)

```
POST /tabs/{tab_id}/scroll
```

Scrolls the page using native mouse wheel events at the specified coordinates. Simulates a real user moving their mouse over an element and scrolling with the mouse wheel.

**Request:**
```json
{
  "x": 100,
  "y": 200,
  "delta_x": 0,
  "delta_y": -300
}
```

**Parameters:**
- `x` (required): X coordinate of the element center to scroll. The mouse wheel event fires at this position.
- `y` (required): Y coordinate of the element center to scroll. The mouse wheel event fires at this position.
- `delta_x` (optional): Horizontal scroll amount in pixels. Positive = scroll right, negative = scroll left. Default: 0.
- `delta_y` (optional): Vertical scroll amount in pixels. Negative = scroll up, positive = scroll down. Default: 0.

At least one of `delta_x` or `delta_y` must be non-zero.

**Note:** Unlike keyboard-based scrolling (PageUp/PageDown), mouse wheel scrolling occurs at a specific coordinate, allowing scrolling of specific scrollable elements (overflow divs, iframes, etc.) by targeting their center point.

---

## Keyboard Actions

### Type Text

```
POST /tabs/{tab_id}/type
```

Types text as if entered by user. Generates keydown, char, and keyup events with a fixed 2ms delay between keystrokes.

**Request:**
```json
{
  "text": "Hello, World!"
}
```

**Response:** Standard action envelope with `result` containing the typed text.

### Press Key

```
POST /tabs/{tab_id}/keyboard/press
```

Presses a key or key combination (keydown + keyup for all keys). Supports keyboard shortcuts via `modifiers` array.

**Request:**
```json
{
  "key": "Enter",
  "modifiers": []
}
```

**Shortcut examples:**
```json
// Copy (Ctrl+C)
{"key": "c", "modifiers": ["Control"]}

// Paste (Ctrl+V)
{"key": "v", "modifiers": ["Control"]}

// Select All (Ctrl+A)
{"key": "a", "modifiers": ["Control"]}

// Undo (Ctrl+Z)
{"key": "z", "modifiers": ["Control"]}

// Redo (Ctrl+Shift+Z)
{"key": "z", "modifiers": ["Control", "Shift"]}

// Save (Ctrl+S)
{"key": "s", "modifiers": ["Control"]}

// Find (Ctrl+F)
{"key": "f", "modifiers": ["Control"]}

// Close tab (Ctrl+W)
{"key": "w", "modifiers": ["Control"]}

// New tab (Ctrl+T)
{"key": "t", "modifiers": ["Control"]}
```

**modifiers options:** `"Shift"`, `"Control"`, `"Alt"`, `"Meta"`

**Common key values:**
- Letters: `"a"` - `"z"`, `"A"` - `"Z"`
- Numbers: `"0"` - `"9"`
- Function keys: `"F1"` - `"F12"`
- Navigation: `"ArrowUp"`, `"ArrowDown"`, `"ArrowLeft"`, `"ArrowRight"`
- Editing: `"Backspace"`, `"Delete"`, `"Enter"`, `"Tab"`, `"Escape"`
- Whitespace: `"Space"`
- Special: `"Home"`, `"End"`, `"PageUp"`, `"PageDown"`, `"Insert"`

### Key Down

```
POST /tabs/{tab_id}/keyboard/down
```

Presses key without releasing.

**Request:**
```json
{
  "key": "Shift",
  "modifiers": []
}
```

### Key Up

```
POST /tabs/{tab_id}/keyboard/up
```

Releases a pressed key.

**Request:**
```json
{
  "key": "Shift"
}
```

---

## JavaScript Execution

### Execute JavaScript

```
POST /tabs/{tab_id}/execute
```

Execute JavaScript in the page context and retrieve results.

**Request:**
```json
{
  "script": "document.querySelectorAll('a').length"
}
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `script` | string | required | JavaScript expression to evaluate |

**Note:** The MCP tool `browser_execute_javascript` uses `expression` as the parameter name and maps it to `script` internally.

**Response:**
```json
{
  "result": {
    "value": 42,
    "type": "number"
  }
}
```

**Supported return types:**
- Primitives: `string`, `number`, `boolean`, `null`, `undefined`
- Objects: Serialized as JSON (must be JSON-serializable)
- Arrays: Serialized as JSON array
- Promises: Resolved value returned (if `await_promise` is true)

**Example expressions:**
```javascript
// Get page data
"document.title"
"window.location.href"
"document.body.innerText.length"

// Query counts
"document.querySelectorAll('button').length"
"document.forms.length"

// Check state
"document.readyState"
"window.scrollY"

// Extract data
"JSON.stringify(Array.from(document.querySelectorAll('h1')).map(h => h.textContent))"

// Application state
"window.APP_STATE?.user?.isLoggedIn ?? false"
```

---

## Screenshots

All screenshots are returned as WebP format at quality 80.

### Viewport Screenshot (Binary)

```
GET /tabs/{tab_id}/screenshot
```

**Query params:**
- `markup=interactive` - Optional element markup overlay (`none`, `interactive`, `clickable`, `typeable`, `inputs`)

**Response:** Binary WebP image data with `Content-Type: image/webp` header.

### Screenshot via Action Envelope

```
POST /tabs/{tab_id}/screenshot
```

Uses the standard action envelope. Screenshots are returned in `screenshot_before` and `screenshot_after` fields.

**Request:**
```json
{
  "screenshot": {
    "markup": "interactive",
    "format": "webp",
    "cursor": true
  }
}
```

**Response:** Standard action envelope with screenshot data in `screenshot_before`/`screenshot_after`.

---

## Text Extraction

### Get Page Text

```
POST /tabs/{tab_id}/text
```

Extracts text content from the page or a specific element.

**Request:**
```json
{
  "selector": ".main-content"
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `selector` | string | No | CSS selector to extract text from. If omitted, returns `document.body.innerText` |

**Response:**
```json
{
  "text": "The extracted text content..."
}
```

Returns `{"text": null}` if the selector matches no element.

---

## Wait

### Duration Wait

```
POST /tabs/{tab_id}/wait
```

Waits for a specified duration. Uses the action envelope, so the page resumes execution during the wait and is paused again afterward.

**Request:**
```json
{
  "ms": 2000
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `ms` | integer | Yes | Duration to wait in milliseconds (0-60000) |

**Response:** Standard action envelope with:
```json
{
  "result": {
    "status": "waited",
    "ms": 2000
  }
}
```

---

## Dialogs (Alerts, Confirms, Prompts)

### Get Pending Dialog

```
GET /tabs/{tab_id}/dialog
```

**Response (dialog present):**
```json
{
  "present": true,
  "dialog_type": "confirm",
  "message": "Are you sure you want to delete this item?",
  "default_prompt": ""
}
```

**Response (no dialog):**
```json
{
  "present": false
}
```

### Accept Dialog

```
POST /tabs/{tab_id}/dialog/accept
```

**Request:**
```json
{
  "prompt_text": "User input for prompt dialogs"
}
```

**Response:**
```json
{
  "success": true
}
```

### Dismiss Dialog

```
POST /tabs/{tab_id}/dialog/dismiss
```

**Response:**
```json
{
  "success": true
}
```

---

## Downloads

Downloads are configured via ABP config at launch (download path, auto-accept behavior). The API provides read-only access to download status.

### List Downloads

```
GET /downloads
```

**Query params:**
- `state=in_progress` - Filter by state: `in_progress`, `completed`, `cancelled`, `failed`
- `limit=100` - Max entries

**Response:**
```json
{
  "downloads": [
    {
      "id": "dl_123",
      "url": "https://example.com/file.pdf",
      "filename": "file.pdf",
      "path": "/downloads/file.pdf",
      "state": "completed",
      "bytes_received": 102400,
      "total_bytes": 102400,
      "mime_type": "application/pdf",
      "start_time": 1699999999000,
      "end_time": 1699999999500
    }
  ]
}
```

### Get Download Status

```
GET /downloads/{download_id}
```

Returns status information about a specific download.

**Response:**
```json
{
  "id": "dl_123",
  "url": "https://example.com/file.pdf",
  "filename": "file.pdf",
  "path": "/downloads/file.pdf",
  "state": "in_progress",
  "bytes_received": 51200,
  "total_bytes": 102400,
  "percent_complete": 50,
  "mime_type": "application/pdf",
  "start_time": 1699999999000
}
```

### Cancel Download

```
POST /downloads/{download_id}/cancel
```

Cancels an in-progress download.

**Response:**
```json
{
  "success": true,
  "message": "Download cancelled"
}
```

---

## File Chooser

File chooser dialogs (triggered by clicking file inputs or save buttons) emit events with unique IDs. Use the file chooser endpoint to provide files to a pending dialog.

### File Chooser Event

When an action triggers a file chooser, the response includes a `file_chooser` event:

```json
{
  "events": [
    {
      "type": "file_chooser",
      "virtual_time_ms": 1699999999200,
      "data": {
        "id": "fc_abc123",
        "tab_id": "tab_xyz789",
        "chooser_type": "open",
        "accepts": [
          {"description": "Images", "extensions": ["jpg", "png", "gif"]},
          {"description": "All Files", "extensions": ["*"]}
        ],
        "multiple": false,
        "pending": true
      }
    }
  ]
}
```

**chooser_type values:** `"open"`, `"open_multiple"`, `"save"`

### Provide Files to File Chooser

```
POST /file-chooser/{chooser_id}
```

Provides files to a pending file chooser dialog.

**Request (for open dialogs):**
```json
{
  "files": [
    "/path/to/document.pdf",
    "/path/to/image.png"
  ]
}
```

**Request (for save dialogs):**
```json
{
  "path": "/path/to/save/output.pdf"
}
```

**Request (to cancel/dismiss):**
```json
{
  "cancel": true
}
```

**Response (files provided):**
```json
{
  "success": true
}
```

**Response (cancelled):**
```json
{
  "success": true,
  "cancelled": true
}
```

**Error (chooser not found or expired):**
```json
{
  "error": "File chooser fc_abc123 not found or already closed"
}
```

---

## History

Session and action history is stored in a SQLite database within the session directory. All history endpoints are under `/api/v1/history/`.

### List Sessions

```
GET /history/sessions
```

Returns all recorded sessions.

### Get Current Session

```
GET /history/sessions/current
```

Returns the currently active session.

### Get Session

```
GET /history/sessions/{session_id}
```

Returns details for a specific session.

### Export Session

```
GET /history/sessions/{session_id}/export
```

Exports a session with all its actions and events.

### List Actions

```
GET /history/actions
```

Returns recorded actions. Supports query filtering.

### Get Action

```
GET /history/actions/{action_id}
```

Returns details for a specific action.

### Get Action Screenshot

```
GET /history/actions/{action_id}/screenshot
```

Returns the screenshot associated with an action as binary image data.

### Delete Actions

```
DELETE /history/actions
```

Deletes action history records.

### List Events

```
GET /history/events
```

Returns recorded events.

### Get Event

```
GET /history/events/{event_id}
```

Returns details for a specific event.

### Delete Events

```
DELETE /history/events
```

Deletes event history records.

### Delete All History

```
DELETE /history
```

Deletes all history (sessions, actions, and events).

---

## Execution Control

Control JavaScript execution and virtual time for deterministic page state between agent actions. When enabled, the page is completely frozen (no JS execution, no timers) between actions.

### Get Execution State

```
GET /tabs/{tab_id}/execution
```

Returns the current execution control state for a tab.

**Response:**
```json
{
  "enabled": true,
  "paused": true,
  "virtual_time_base_ms": 1700000000000.0
}
```

| Field | Type | Description |
|-------|------|-------------|
| `enabled` | boolean | Whether execution control is enabled for this tab |
| `paused` | boolean | Whether JS execution is currently paused |
| `virtual_time_base_ms` | number | Virtual time base in milliseconds since epoch |

### Set Execution State

```
POST /tabs/{tab_id}/execution
```

Enable execution control and/or pause/resume JavaScript execution.

**Request:**
```json
{
  "paused": true,
  "initial_virtual_time": 1700000000.0
}
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `paused` | boolean | Yes | `true` to pause, `false` to resume |
| `initial_virtual_time` | number | No | Initial virtual time in seconds since Unix epoch (only used when first enabling) |

**Response:**
```json
{
  "enabled": true,
  "paused": true,
  "virtual_time_base_ms": 1700000000000.0
}
```

### How It Works

Execution control uses two CDP mechanisms:

1. **Debugger.pause/resume** - Halts/resumes all JavaScript execution
2. **Emulation.setVirtualTimePolicy** - Freezes/advances timers, Date.now(), and animations

By default (unless `--abp-disable-pause` is set):

1. Actions (click, type, navigate) automatically **resume** execution before dispatching
2. After the action completes, execution is **paused** before taking screenshots
3. Screenshots capture a completely frozen page state

**Action Flow:**
```
Agent sends click action
  → ABP resumes JS (Debugger.resume + virtual time advance)
  → Takes before screenshot (page state before action)
  → Dispatches click event
  → Waits for action to complete
  → Takes after screenshot (page state after action)
  → Pauses JS (virtual time pause + Debugger.pause)
  → Returns response with both screenshots
```

### Command-Line Flag

Execution control is **enabled by default** when using ABP. To disable it:
```bash
./chrome --enable-abp --abp-disable-pause
```

With `--abp-disable-pause`, actions do not pause/resume execution - they behave without freezing the page.

### Use Cases

- **Deterministic testing**: Ensure identical page state between test runs
- **AI agent reliability**: Freeze timers/animations while agent processes screenshot
- **Debugging**: Stop page execution to inspect state

---

## Error Codes

| Code | Description |
|------|-------------|
| `INVALID_REQUEST` | Malformed request body |
| `TAB_NOT_FOUND` | Tab ID does not exist |
| `ELEMENT_NOT_FOUND` | Element ID does not exist or is stale |
| `SELECTOR_NOT_FOUND` | No element matches selector |
| `TIMEOUT` | Operation timed out |
| `NAVIGATION_FAILED` | Navigation could not complete |
| `ELEMENT_NOT_VISIBLE` | Element exists but is not visible |
| `ELEMENT_NOT_INTERACTABLE` | Element cannot receive input |
| `DIALOG_NOT_PRESENT` | No dialog to accept/dismiss |
| `FILE_CHOOSER_NOT_PRESENT` | No file chooser dialog to handle |
| `FILE_NOT_FOUND` | Specified file path does not exist |
| `FILE_NOT_READABLE` | File exists but cannot be read |
| `DOWNLOAD_NOT_FOUND` | Download ID does not exist |
| `DOWNLOAD_FAILED` | Download could not complete |
| `NETWORK_ERROR` | Network operation failed |
| `EVALUATION_ERROR` | JavaScript evaluation failed |
| `UNAUTHORIZED` | Missing or invalid auth token |
| `RATE_LIMITED` | Too many requests |
| `NOT_READY` | ABP is still initializing, poll /browser/status |
| `BROWSER_INIT_FAILED` | Browser window failed to initialize |
| `DEVTOOLS_INIT_FAILED` | DevTools connection failed to establish |
