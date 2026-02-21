# ABP MCP Server Reference

The Agent Browser Protocol (ABP) includes an embedded MCP server -- no separate process, no Node.js, no bridge. The MCP endpoint runs on the same port as the REST API (`localhost:8222`) and starts automatically with `--enable-abp`. It implements the [Model Context Protocol](https://modelcontextprotocol.io/) Streamable HTTP transport (protocol version `2025-03-26`) and exposes 12 tools for complete browser control.

---

## Setup

### Claude Code (recommended)

One command:

```bash
claude mcp add browser --transport streamable-http --url http://localhost:8222/mcp
```

### Claude Desktop

Add to your `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "browser": {
      "transport": "streamable-http",
      "url": "http://localhost:8222/mcp"
    }
  }
}
```

### OpenAI Codex

Add to your MCP configuration:

```json
{
  "mcpServers": {
    "browser": {
      "transport": "streamable-http",
      "url": "http://localhost:8222/mcp"
    }
  }
}
```

### Generic MCP Client

Any client that supports Streamable HTTP can connect. Use raw JSON-RPC over HTTP:

```bash
# 1. Initialize the session
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-03-26",
      "clientInfo": {"name": "my-agent", "version": "1.0"},
      "capabilities": {}
    }
  }'

# 2. List available tools
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/list",
    "params": {}
  }'

# 3. Call a tool
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "browser_get_status",
      "arguments": {}
    }
  }'
```

---

## Tools

ABP exposes 12 MCP tools. All `tab_id` parameters are optional and default to the active tab.

### 1. browser_action

Execute 1--3 batched browser input actions in a single turn. One screenshot is returned after all actions complete. The page is paused between tool calls; JS and animations only run during execution.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `actions` | array | Yes | Array of 1--3 action objects (see variants below) |
| `tab_id` | string | No | Target tab ID (defaults to active tab) |
| `screenshot` | object | No | Screenshot config: `markup` (array), `disable_markup` (array), `format` (string) |

**Action variants:**

Each action object has a `type` field that determines its schema:

**`mouse_click`** -- Click at coordinates.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | `"mouse_click"` | Yes | Action discriminator |
| `x` | number | Yes | X pixel coordinate |
| `y` | number | Yes | Y pixel coordinate |
| `button` | string | No | `left`, `right`, or `middle` (default: `left`) |
| `click_count` | number | No | Number of clicks (default: 1; use 2 for double-click) |
| `modifiers` | array | No | Modifier keys: `SHIFT`, `CONTROL`, `ALT`, `META` |

**`keyboard_type`** -- Type text.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | `"keyboard_type"` | Yes | Action discriminator |
| `text` | string | Yes | Text to type |

**`keyboard_press`** -- Press a key or key combo.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | `"keyboard_press"` | Yes | Action discriminator |
| `key` | string | Yes | Key name in ALL-CAPS (`ENTER`, `TAB`, `ESCAPE`, `ARROWUP`, etc.) |
| `modifiers` | array | No | Modifier keys: `SHIFT`, `CONTROL`, `ALT`, `META` |
| `action` | string | No | `press` (default), `down`, or `up` |

**`mouse_hover`** -- Move mouse to coordinates (hover).

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | `"mouse_hover"` | Yes | Action discriminator |
| `x` | number | Yes | X pixel coordinate |
| `y` | number | Yes | Y pixel coordinate |

**`mouse_drag`** -- Drag from one point to another.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | `"mouse_drag"` | Yes | Action discriminator |
| `start_x` | number | Yes | Start X coordinate |
| `start_y` | number | Yes | Start Y coordinate |
| `end_x` | number | Yes | End X coordinate |
| `end_y` | number | Yes | End Y coordinate |
| `steps` | number | No | Number of intermediate points (default: 10) |

**Notes:**
- Key names are ALL-CAPS: `ENTER`, `TAB`, `ESCAPE`, `BACKSPACE`, `ARROWUP`, `ARROWDOWN`, etc.
- Abbreviations accepted: `CTRL`, `CMD`, `ESC`, `DEL`, `BS`, `CR`, `UP`, `DOWN`, `LEFT`, `RIGHT`.
- Batch actions that form a single user intent (e.g., click + type + press Enter = 3 actions, 1 tool call).
- Actions execute sequentially with a 20ms pause between each.

**Example -- click a search box, type a query, press Enter:**

```json
{
  "actions": [
    {"type": "mouse_click", "x": 350, "y": 200},
    {"type": "keyboard_type", "text": "weather today"},
    {"type": "keyboard_press", "key": "ENTER"}
  ]
}
```

---

### 2. browser_scroll

Scroll using mouse wheel at a target element's coordinates.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `x` | number | Yes | X pixel coordinate where mouse wheel fires |
| `y` | number | Yes | Y pixel coordinate where mouse wheel fires |
| `delta_x` | number | No | Horizontal scroll in pixels (positive=right, negative=left, default: 0) |
| `delta_y` | number | No | Vertical scroll in pixels (positive=down, negative=up, default: 0) |

**Notes:**
- At least one of `delta_x` or `delta_y` must be non-zero.
- Target the center of the scrollable element with `x`, `y`.
- Do not batch scrolling into `browser_action` -- use this tool separately.

---

### 3. browser_navigate

Navigate to a URL, or go back/forward/reload.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `url` | string | No | URL to navigate to |
| `action` | string | No | Navigation action: `back`, `forward`, or `reload` |

**Notes:**
- Provide either `url` or `action`, not both.
- If neither is provided, the tool returns an error.

---

### 4. browser_screenshot

Take a screenshot of the current viewport. Also acts as a wait: resumes page execution, waits for rendering to settle, captures the viewport, then re-pauses execution.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `markup` | array | No | Markup overlays to enable (none by default): `clickable`, `typeable`, `scrollable`, `grid`, `selected` |
| `disable_markup` | array | No | Markup overlays to disable (all enabled by default): `clickable`, `typeable`, `scrollable`, `grid`, `selected` |
| `format` | string | No | Image format: `png`, `webp`, or `jpeg` |

**Markup overlay types:**

| Overlay | Color | Description |
|---------|-------|-------------|
| `clickable` | Green | Clickable elements (links, buttons) |
| `typeable` | Orange | Text input fields |
| `scrollable` | Purple dashed | Scrollable containers |
| `grid` | Red | Coordinate grid for targeting |
| `selected` | Blue | Currently focused element |

**Notes:**
- Use `browser_screenshot` to wait for slow content (AJAX, animations, redirects). It runs the full resume-wait-capture-pause cycle without performing any action.
- When both `markup` and `disable_markup` are provided, `markup` takes priority.

---

### 5. browser_tabs

Manage browser tabs. Default action is `list`.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | No | Tab action: `list` (default), `new`, `close`, `info`, `activate`, `stop` |
| `tab_id` | string | No | Target tab ID (required for `close`, `info`, `activate`, `stop`) |
| `url` | string | No | URL for `new` action (opens `about:blank` if omitted) |

**Action details:**

| Action | Description |
|--------|-------------|
| `list` | List all open tabs with their IDs, URLs, and titles |
| `new` | Create a new tab, optionally navigating to `url` |
| `close` | Close the specified tab |
| `info` | Get detailed information about a tab |
| `activate` | Switch to (focus) the specified tab |
| `stop` | Stop loading the specified tab |

---

### 6. browser_javascript

Execute JavaScript in the page context.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `expression` | string | Yes | JavaScript expression to evaluate |

**Notes:**
- Prefer `browser_action` (click, type, scroll) for all user interactions.
- Use `browser_javascript` only for: (1) extracting data from the page (reading attributes, counting elements), or (2) inspecting the DOM when a mouse/keyboard action did not produce the expected result.
- The MCP parameter is `expression`; this maps to the REST API `script` parameter internally.

---

### 7. browser_text

Get the visible text content of the page.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `selector` | string | No | CSS selector to scope text extraction (returns full page text if omitted) |

---

### 8. browser_dialog

Handle browser dialogs (alert, confirm, prompt, beforeunload).

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `action` | string | No | Dialog action: `check` (default), `accept`, `dismiss` |
| `prompt_text` | string | No | Text to enter for prompt dialogs (used with `accept`) |

**Action details:**

| Action | Description |
|--------|-------------|
| `check` | Check if a dialog is pending and return its info |
| `accept` | Accept the dialog (optionally with `prompt_text` for prompt dialogs) |
| `dismiss` | Dismiss (cancel) the dialog |

---

### 9. browser_downloads

Manage downloads.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | No | Download action: `list` (default), `status`, `cancel` |
| `download_id` | string | No | Download ID (required for `status` and `cancel`) |
| `state` | string | No | Filter by state (for `list`): `in_progress`, `completed`, `cancelled`, `failed` |
| `limit` | number | No | Maximum number of downloads to return (for `list`) |

---

### 10. browser_files

Provide files to a pending file chooser dialog.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `chooser_id` | string | Yes | File chooser ID (from the file chooser event) |
| `files` | array | No | Array of file paths to provide (strings) |
| `path` | string | No | Save path (for save dialogs) |
| `cancel` | boolean | No | Cancel the file chooser instead of providing files |

---

### 11. browser_get_status

Get browser status and readiness. No parameters.

Returns the browser's current state, including whether it is ready to accept commands, the number of open tabs, and the session directory path.

---

### 12. browser_shutdown

Gracefully shut down the browser.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `timeout_ms` | number | No | Timeout in milliseconds before force quit |

---

## Architecture

The MCP server is embedded inside the Chromium process alongside the REST API. Both share the same HTTP server and request handler.

```
+-------------------------------------------------------------+
|                    AI Agent / LLM                            |
+---------------------------+---------------------------------+
                            | JSON-RPC over HTTP (MCP)
                            | or REST API (GET/POST/DELETE)
                            v
+-------------------------------------------------------------+
|  AbpHttpServer (IO thread)                     Port 8222    |
|  +-- /api/v1/*           -> REST API routes                 |
|  +-- /mcp                -> AbpMcpHandler (MCP JSON-RPC)    |
+---------------------------+---------------------------------+
                            | PostTask to UI thread
                            v
+-------------------------------------------------------------+
|  AbpController (UI thread)                                  |
|  - Shared handler for both REST and MCP requests            |
|  - Direct access to Browser, TabStripModel                  |
|  - Uses DevToolsAgentHost for CDP commands                  |
|  - AbpInputDispatcher for native mouse/keyboard input       |
|  - AbpActionContext for action lifecycle (pause/resume)     |
+-------------------------------------------------------------+
```

**How MCP tools dispatch:**

1. `AbpMcpHandler` receives a `tools/call` JSON-RPC request.
2. It routes by tool name to a `Call*` method (e.g., `CallBrowserAction`).
3. The `Call*` method translates MCP arguments into a REST API path + body.
4. It calls `AbpController::HandleRequest(method, path, body, callback)` -- the same handler the REST API uses.
5. The REST response is converted back into an MCP tool result (JSON text + optional image content blocks for screenshots).

**Benefits of the embedded design:**

- **Single process** -- no Node.js dependency, no IPC, no bridge process to manage.
- **Single port** -- REST and MCP share `localhost:8222`.
- **Lower latency** -- MCP tool calls dispatch directly to the controller via function calls, not additional HTTP round-trips.
- **Simpler deployment** -- start the browser and both protocols are available immediately.

---

## Protocol Details

| Property | Value |
|----------|-------|
| Transport | Streamable HTTP |
| Protocol version | `2025-03-26` |
| Endpoint | `http://localhost:8222/mcp` |
| Content-Type | `application/json` |
| Session header | `Mcp-Session-Id` (returned after initialize) |

### HTTP Methods

| Method | Path | Description |
|--------|------|-------------|
| POST | `/mcp` | Send JSON-RPC messages (requests, notifications) |
| GET | `/mcp` | Open SSE stream for server-initiated messages (not yet implemented) |
| DELETE | `/mcp` | Terminate session |

### Initialization Flow

```
Client                                  Server (ABP)
  |                                        |
  |  POST /mcp                             |
  |  {"jsonrpc":"2.0","id":1,              |
  |   "method":"initialize",               |
  |   "params":{                           |
  |     "protocolVersion":"2025-03-26",    |
  |     "clientInfo":{...},                |
  |     "capabilities":{}                  |
  |   }}                                   |
  | -------------------------------------> |
  |                                        |
  |  200 OK                                |
  |  {"jsonrpc":"2.0","id":1,              |
  |   "result":{                           |
  |     "protocolVersion":"2025-03-26",    |
  |     "serverInfo":{                     |
  |       "name":"abp-browser",            |
  |       "version":"1.0.0"},              |
  |     "capabilities":{"tools":{}}        |
  |   }}                                   |
  | <------------------------------------- |
  |                                        |
  |  POST /mcp                             |
  |  {"jsonrpc":"2.0",                     |
  |   "method":"notifications/initialized"}|
  | -------------------------------------> |
  |                                        |
  |  202 Accepted                          |
  | <------------------------------------- |
```

### JSON-RPC Error Codes

| Code | Meaning |
|------|---------|
| -32700 | Parse error (invalid JSON) |
| -32600 | Invalid JSON-RPC request |
| -32601 | Method or tool not found |
| -32602 | Invalid or missing parameters |
| -32000 | Tab not found |
| -32001 | Navigation failed |
| -32002 | Operation timed out |
| -32003 | JavaScript evaluation error |
