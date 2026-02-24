# Plan: Consolidate MCP Tools from 33 to 12

## Context

ABP currently exposes 33 MCP tools. LLMs perform better with fewer, clearer tools — less scanning, less confusion about which tool to pick. Magnitude achieves 94% on WebVoyager with 15 actions batched into an array per turn. This plan consolidates the MCP tool surface and adds a batch REST endpoint for executing multiple in-page events within a single action lifecycle.

**Scope**: `abp_mcp_handler.cc/h` for MCP tools, `abp_controller.cc/h` and `abp_http_server.cc/h` for the new batch REST endpoint.

## Design: `browser_action` Batch Tool

### Motivation

The most common agent pattern is click → type → press Enter. With individual tools, this requires 3 MCP round-trips (~500ms-1s each in LLM generation + network). Batching into a single tool call eliminates that overhead.

Magnitude validates this pattern — their agent returns an array of actions per LLM turn and achieves SOTA results. The observe→act→observe loop happens between LLM turns, not between individual actions within a batch.

### MCP Tool Schema

```json
{
  "name": "browser_action",
  "description": "Execute one or more browser actions. Each action has a 'type' and type-specific params. Actions execute sequentially within a single action lifecycle with a 20ms pause between each. Screenshot is taken once after all actions complete.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "actions": {
        "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "type": {
              "type": "string",
              "enum": ["mouse_click", "keyboard_type", "keyboard_press", "mouse_hover", "mouse_drag"]
            },
            "x": { "type": "number" },
            "y": { "type": "number" },
            "text": { "type": "string" },
            "key": { "type": "string" },
            "button": { "type": "string", "enum": ["left", "right", "middle"] },
            "click_count": { "type": "number" },
            "modifiers": { "type": "array", "items": { "type": "string", "enum": ["Shift", "Control", "Alt", "Meta"] } },
            "start_x": { "type": "number" },
            "start_y": { "type": "number" },
            "end_x": { "type": "number" },
            "end_y": { "type": "number" },
            "steps": { "type": "number" },
            "action": { "type": "string", "enum": ["press", "down", "up"] }
          },
          "required": ["type"]
        },
        "minItems": 1,
        "maxItems": 3
      },
      "tab_id": { "type": "string" },
      "screenshot": {
        "type": "object",
        "properties": {
          "markup": { "type": "array", "items": { "type": "string", "enum": ["clickable", "typeable", "scrollable", "grid", "selected"] } },
          "disable_markup": { "type": "array", "items": { "type": "string" } },
          "format": { "type": "string", "enum": ["png", "webp", "jpeg"] }
        }
      }
    },
    "required": ["actions"]
  }
}
```

### Action Types and Required Params

| type | Required | Optional |
|------|----------|----------|
| `mouse_click` | `x`, `y` | `button`, `click_count`, `modifiers` |
| `keyboard_type` | `text` | |
| `keyboard_press` | `key` | `action` (press/down/up, default press), `modifiers` |
| `mouse_hover` | `x`, `y` | |
| `mouse_drag` | `start_x`, `start_y`, `end_x`, `end_y` | `steps` |

### Example: Click, Type, Submit

```json
{
  "actions": [
    {"type": "mouse_click", "x": 450, "y": 320},
    {"type": "keyboard_type", "text": "hello world"},
    {"type": "keyboard_press", "key": "ENTER"}
  ],
  "screenshot": {"markup": ["clickable", "grid"]}
}
```

### Execution Semantics

- All actions execute within a **single action lifecycle** (one pause→resume→execute→screenshot cycle)
- **20ms pause** between each action (allows compositor/renderer to settle without a full action lifecycle round-trip)
- Screenshot is taken **once** after all actions complete
- **All actions are attempted** — no early abort on runtime errors
- **Upfront validation only** — before executing anything, validate all actions for obvious errors (coordinates outside viewport for mouse_click/mouse_hover/mouse_drag). If validation fails, return error immediately without executing any actions.
- **Max 3 actions** per batch (covers the dominant click→type→submit pattern without encouraging blind sequences)

### Validation (pre-execution)

Validate before any action executes:
- All actions have a valid `type`
- Actions requiring coordinates (`mouse_click`, `mouse_hover`, `mouse_drag`) have coordinates within the viewport bounds
- Required params are present for each action type (`text` for keyboard_type, `key` for keyboard_press, etc.)
- `keyboard_press`: normalize `key` and `modifiers` (see Key Normalization below), then validate against the allowed key set

If validation fails, return error immediately with no side effects:

```json
{
  "error": "action 1: mouse_click coordinates (9999, 9999) outside viewport (1035x836)"
}
```

```json
{
  "error": "action 0: keyboard_press invalid key 'FOOBAR' — see valid keys in tool description"
}
```

### Key Normalization

All `key` and `modifiers` values are uppercased and common abbreviations are expanded before validation and dispatch. This runs in both the REST `/batch` endpoint and the MCP `browser_action` handler.

**Abbreviation map:**

| Input (case-insensitive) | Normalized |
|--------------------------|------------|
| `CTRL`, `⌃` | `CONTROL` |
| `CMD`, `COMMAND`, `META`, `⌘` | `META` |
| `OPT`, `OPTION`, `⌥` | `ALT` |
| `⇧` | `SHIFT` |
| `ESC` | `ESCAPE` |
| `DEL` | `DELETE` |
| `BS`, `BACKSPACE` | `BACKSPACE` |
| `CR`, `RETURN` | `ENTER` |
| `INS` | `INSERT` |
| `PGUP` | `PAGEUP` |
| `PGDN`, `PGDOWN` | `PAGEDOWN` |
| `UP` | `ARROWUP` |
| `DOWN` | `ARROWDOWN` |
| `LEFT` | `ARROWLEFT` |
| `RIGHT` | `ARROWRIGHT` |

Single characters (`A`-`Z`, `0`-`9`) are kept as-is after uppercasing.

**Valid keys** (after normalization):

```
Letters:      A-Z
Digits:       0-9
Function:     F1-F24
Navigation:   ARROWUP, ARROWDOWN, ARROWLEFT, ARROWRIGHT, HOME, END, PAGEUP, PAGEDOWN
Editing:      BACKSPACE, DELETE, INSERT, ENTER, TAB, ESCAPE, SPACE
Modifiers:    SHIFT, CONTROL, ALT, META
Symbols:      Mapped from DOM key values — e.g. COMMA, PERIOD, SLASH, BACKSLASH,
              SEMICOLON, QUOTE, BRACKETLEFT, BRACKETRIGHT, MINUS, EQUAL, BACKQUOTE
```

**Implementation**: A static `base::flat_map<std::string, std::string>` for abbreviations, and a `base::flat_set<std::string>` for the valid key set. Both built once. `NormalizeKey(const std::string& input) -> std::optional<std::string>` returns the normalized key or `std::nullopt` if invalid.

## REST API: Batch Endpoint

### `POST /api/v1/tabs/{id}/batch`

New REST endpoint that executes multiple events within a single action lifecycle.

**Request:**
```json
{
  "actions": [
    {"type": "mouse_click", "x": 450, "y": 320},
    {"type": "keyboard_type", "text": "hello world"},
    {"type": "keyboard_press", "key": "ENTER"}
  ],
  "screenshot": {
    "markup": "interactive",
    "format": "webp"
  }
}
```

**Response (success):**
```json
{
  "action_id": "abc123",
  "actions_executed": 3,
  "screenshot": "base64...",
  "scroll_position": {"x": 0, "y": 150},
  "viewport_size": {"width": 1035, "height": 836}
}
```

**Response (validation error — nothing executed):**
```json
{
  "error": "action 1: mouse_click coordinates (9999, 9999) outside viewport (1035x836)"
}
```

### Execution Flow in AbpController

```
POST /api/v1/tabs/{id}/batch
  │
  ▼
AbpController::HandleBatchRequest()
  │
  ├─ Validate ALL actions upfront:
  │   ├─ Valid type?
  │   ├─ Required params present?
  │   ├─ Coordinates within viewport?
  │   └─ If any fail → return 400 error immediately, execute nothing
  │
  ├─ Start single action lifecycle (pause → resume)
  │
  ├─ For each action in array:
  │   ├─ Dispatch via AbpInputDispatcher (mouse_click/keyboard_type/keyboard_press/mouse_hover/mouse_drag)
  │   └─ Wait 20ms (base::SingleThreadTaskRunner::PostDelayedTask)
  │
  ├─ After last action:
  │   ├─ ForceRedraw + capture screenshot
  │   └─ Re-pause (end action lifecycle)
  │
  └─ Return JSON response
```

The 20ms inter-action delay uses `PostDelayedTask` on the UI thread. This is enough for:
- Compositor to process input events and update layout
- Focus changes to propagate (click → type needs focus to land)
- Scroll position to settle

It is NOT a full action lifecycle round-trip (which would be ~200-300ms with ForceRedraw + virtual time transitions).

## New Tool Surface (11 tools)

### Core (2 tools)

| # | Tool | Old Tools | Description |
|---|------|-----------|-------------|
| 1 | `browser_action` | `browser_click`, `browser_type`, `browser_keyboard_press/down/up`, `browser_mouse_move`, `browser_drag` | Array of 1-3 actions with shared screenshot |
| 2 | `browser_scroll` | `browser_scroll` | `x, y, delta_x?, delta_y?` — standalone, never batched |

### Navigation & Observation (3 tools)

| # | Tool | Old Tools | Key Params |
|---|------|-----------|------------|
| 3 | `browser_navigate` | `browser_navigate`, `browser_go_back`, `browser_go_forward`, `browser_reload` | `url?, action?: back\|forward\|reload` |
| 4 | `browser_screenshot` | `browser_screenshot` | `disable_markup?, markup?, format?` |
| 5 | `browser_tabs` | `browser_list_tabs`, `browser_new_tab`, `browser_close_tab`, `browser_get_tab_info`, `browser_activate_tab`, `browser_stop_loading` | `action?: list\|new\|close\|info\|activate\|stop (default list), tab_id?, url?` |

### Content (2 tools)

| # | Tool | Old Tools | Key Params |
|---|------|-----------|------------|
| 6 | `browser_javascript` | `browser_execute_javascript` | `expression` |
| 7 | `browser_text` | `browser_get_text` | `selector?` |

### Situational (3 tools — low frequency)

| # | Tool | Old Tools | Key Params |
|---|------|-----------|------------|
| 8 | `browser_dialog` | `browser_get_dialog`, `browser_accept_dialog`, `browser_dismiss_dialog` | `action?: check\|accept\|dismiss (default check), prompt_text?` |
| 9 | `browser_downloads` | `browser_list_downloads`, `browser_get_download`, `browser_cancel_download` | `action?: list\|status\|cancel (default list), download_id?, state?, limit?` |
| 10 | `browser_files` | `browser_provide_files` | `chooser_id, files?, path?, cancel?` |

### Browser Management (2 tools)

| # | Tool | Old Tools | Key Params |
|---|------|-----------|------------|
| 11 | `browser_get_status` | `browser_get_status` | (none) |
| 12 | `browser_shutdown` | `browser_shutdown` | `timeout_ms?` |

### Removed from MCP (still available via REST)

| Old Tool | Reason |
|----------|--------|
| `browser_get_session_data` | Debugging tool, not needed during agent operation |
| `browser_get_execution_state` | Execution control is automatic in action context |
| `browser_set_execution_state` | Execution control is automatic in action context |

## Consolidation Breakdown

```
33 original → 12 new (64% reduction)

3 tools dropped entirely (REST only)
8×input → 1 (browser_action with actions array — mouse_click, keyboard_type, keyboard_press, mouse_hover, mouse_drag)
scroll kept as standalone tool (browser_scroll)
4×navigation → 1 (browser_navigate with url|action)
6×tabs → 1 (browser_tabs with action param)
3×dialog → 1 (browser_dialog with action param)
3×downloads → 1 (browser_downloads with action param)
4 unchanged (screenshot, javascript, text, files, get_status, shutdown)
```

## Implementation Steps

### Step 1: Add batch REST endpoint to AbpHttpServer and AbpController

**`abp_http_server.cc`** — Add route for `POST /api/v1/tabs/{id}/batch`:
```cpp
if (method == "POST" && MatchPath(path, "/api/v1/tabs/*/batch", &tab_id)) {
  controller_->HandleBatchRequest(tab_id, body, std::move(callback));
  return;
}
```

**`abp_controller.h`** — Add declarations:
```cpp
void HandleBatchRequest(const std::string& tab_id,
                        const std::string& body,
                        HttpCallback callback);
void ExecuteBatchActions(const std::string& tab_id,
                         base::Value::List actions,
                         int current_index,
                         HttpCallback callback);
```

**`abp_controller.cc`** — Implement batch execution:

`HandleBatchRequest` parses the JSON body, extracts the `actions` array and `screenshot` config, then for each action: normalizes `keyboard_press` key/modifiers (uppercase + abbreviation expansion via `NormalizeKey()`), validates all actions upfront (types, required params, coordinate bounds, valid key names), then starts a single action lifecycle (pause→resume) and calls `ExecuteBatchActions` for the first action.

`ExecuteBatchActions` is a recursive async method:
1. Dispatch action at `current_index` via existing input methods (same code paths as individual REST endpoints)
2. If more actions remain → `PostDelayedTask(ExecuteBatchActions, 20ms)` with `current_index + 1`
3. If last action → ForceRedraw + capture screenshot + re-pause + return success response

All validation happens before execution starts. Once the batch begins, all actions run to completion.

Each action dispatches through the same code paths used by individual endpoints:
- `mouse_click` → `DispatchClickEvent()` (same as `HandleClickRequest`)
- `keyboard_type` → `DispatchTypeEvent()` (same as `HandleTypeRequest`)
- `keyboard_press` → `DispatchKeyEvent()` (same as `HandleKeyboardPressRequest` etc.)
- `mouse_hover` → `DispatchMouseMoveEvent()` (same as `HandleMoveRequest`)
- `mouse_drag` → `DispatchDragEvent()` (same as `HandleDragRequest`)

### Step 2: Update MCP tool definitions in `GetToolDefinitions()`

Replace 33 `ToolBuilder` blocks with 12. The `browser_action` tool uses a custom schema for the actions array.

**`browser_action`:**
```cpp
tools.Append(ToolBuilder("browser_action")
    .Description(
        "Execute one or more browser actions (max 3). Each action has a "
        "'type' and type-specific params. Actions run sequentially with "
        "a 20ms pause between each. Screenshot is taken once after all "
        "actions complete.\n\n"
        "Batch these common workflows:\n"
        "- mouse_click → keyboard_type → keyboard_press(ENTER) — click field, type text, submit\n"
        "- mouse_click → keyboard_type — click field, type text\n"
        "keyboard_press handles key combos: key:'A', modifiers:['CONTROL'] for Ctrl+A.\n"
        "Use a single action for standalone clicks, keypresses, etc.\n"
        "Use browser_scroll for scrolling (not part of this tool).\n\n"
        "Action params by type:\n"
        "- mouse_click: x, y, button?, click_count?, modifiers?\n"
        "- keyboard_type: text\n"
        "- keyboard_press: key (ENTER, TAB, ESCAPE, A-Z, F1-F12, ARROWUP, etc.), "
        "modifiers? ([SHIFT, CONTROL, ALT, META]), action? (press|down|up). "
        "Common abbreviations accepted: CTRL→CONTROL, CMD→META, ESC→ESCAPE, DEL→DELETE.\n"
        "- mouse_hover: x, y\n"
        "- mouse_drag: start_x, start_y, end_x, end_y, steps?")
    .OptionalString("tab_id", "Target tab ID")
    .RequiredArray("actions", "Array of action objects", /* item schema built inline */)
    .OptionalObject("screenshot", "Screenshot config after actions complete")
    .Build());
```

**`browser_navigate`:**
```cpp
tools.Append(ToolBuilder("browser_navigate")
    .Description(
        "Navigate to a URL, or go back/forward/reload. "
        "Provide 'url' to navigate, or 'action' for back/forward/reload.")
    .OptionalString("tab_id", "Target tab ID")
    .OptionalString("url", "URL to navigate to")
    .OptionalStringEnum("action", "Navigation action",
                        {"back", "forward", "reload"})
    .Build());
```

**`browser_tabs`:**
```cpp
tools.Append(ToolBuilder("browser_tabs")
    .Description(
        "Manage browser tabs. Default: list all tabs. "
        "Actions: list, new (create tab), close, info (tab details), "
        "activate (switch to tab), stop (stop loading).")
    .OptionalStringEnum("action", "Tab action",
                        {"list", "new", "close", "info", "activate", "stop"})
    .OptionalString("tab_id", "Target tab ID (for close/info/activate/stop)")
    .OptionalString("url", "URL for new tab")
    .Build());
```

**`browser_keyboard`** (consolidated key tool within `browser_action`, also available via `browser_navigate` etc. — the remaining tools follow the same pattern as the previous plan).

### Step 3: Replace Call* methods in the header

In `abp_mcp_handler.h`, replace 33 `Call*` declarations with 12:

```cpp
void CallBrowserAction(...);       // NEW: batched input actions
void CallBrowserScroll(...);       // kept as standalone tool
void CallBrowserNavigate(...);     // NEW: handles url + back/forward/reload
void CallBrowserScreenshot(...);
void CallBrowserTabs(...);         // NEW: replaces 6 tab tools
void CallBrowserJavascript(...);   // renamed from ExecuteJavascript
void CallBrowserText(...);         // renamed from GetText
void CallBrowserDialog(...);       // NEW: replaces 3 dialog tools
void CallBrowserDownloads(...);    // NEW: replaces 3 download tools
void CallBrowserFiles(...);        // renamed from ProvideFiles
void CallBrowserGetStatus(...);    // kept as-is
void CallBrowserShutdown(...);     // kept as-is
```

### Step 4: Implement `CallBrowserAction` in MCP handler

`CallBrowserAction` translates the MCP tool call into a `POST /api/v1/tabs/{id}/batch` request body and delegates to `AbpController::HandleBatchRequest`. The screenshot config from the MCP params maps directly to the REST body's `screenshot` field.

### Step 5: Implement remaining consolidated Call* methods

Same as previous plan:

**`CallBrowserNavigate`** — reads `url` or `action` param:
- If `url` provided → POST `/api/v1/tabs/{id}/navigate`
- If `action == "back"` → POST `/api/v1/tabs/{id}/back`
- If `action == "forward"` → POST `/api/v1/tabs/{id}/forward`
- If `action == "reload"` → POST `/api/v1/tabs/{id}/reload`
- If neither → error "Provide 'url' or 'action'"

**`CallBrowserTabs`** — reads `action` param (default "list"):
- `"list"` → GET `/api/v1/tabs`
- `"new"` → existing `CallBrowserNewTab` logic
- `"close"` → DELETE `/api/v1/tabs/{id}`
- `"info"` → GET `/api/v1/tabs/{id}`
- `"activate"` → POST `/api/v1/tabs/{id}/activate`
- `"stop"` → POST `/api/v1/tabs/{id}/stop`

**`CallBrowserDialog`** — reads `action` param (default "check"):
- `"check"` → GET `/api/v1/tabs/{id}/dialog`
- `"accept"` → POST `/api/v1/tabs/{id}/dialog/accept`
- `"dismiss"` → POST `/api/v1/tabs/{id}/dialog/dismiss`

**`CallBrowserDownloads`** — reads `action` param (default "list"):
- `"list"` → GET `/api/v1/downloads`
- `"status"` → GET `/api/v1/downloads/{download_id}`
- `"cancel"` → POST `/api/v1/downloads/{download_id}/cancel`

### Step 6: Update `HandleToolsCall` dispatch table

Replace the 33-entry if/else chain with 12 entries matching the new tool names.

### Step 7: Update `kGuideContent` resource

```
## Tool Reference

**Actions:** `browser_action` — execute 1-3 input actions in a single call
  Types: mouse_click (x, y), keyboard_type (text),
  keyboard_press (key, modifiers? — e.g. ENTER, TAB, ESCAPE, or key:'A' modifiers:['CONTROL'] for Ctrl+A),
  mouse_hover (x, y), mouse_drag (start_x, start_y, end_x, end_y)
  Keys are ALL-CAPS. Abbreviations accepted: CTRL, CMD, ESC, DEL, etc.

  Batch these workflows:
    mouse_click → keyboard_type → keyboard_press(ENTER) — click input, type text, submit
    mouse_click → keyboard_type                          — click input, type text
  Screenshot is taken once after all actions complete.

**Scroll:** `browser_scroll` (x, y, delta_x?, delta_y?) — always standalone

**Navigation:** `browser_navigate` (url or action: back|forward|reload)

**Observation:** `browser_screenshot` (markup, format), `browser_text` (selector?)

**Content:** `browser_javascript` (expression)

**Tabs:** `browser_tabs` (action: list|new|close|info|activate|stop)

**Situational:** `browser_dialog` (action: check|accept|dismiss),
`browser_downloads` (action: list|status|cancel), `browser_files` (chooser_id)

**Browser:** `browser_get_status`, `browser_shutdown` (timeout_ms?)
```

### Step 8: Update skill file

Update `tools/abp-claude-skill/abp-browser.md` to reference new tool names and document the batch pattern.

## ToolBuilder Extensions

The `browser_action` schema requires array and nested object support in `AbpToolBuilder`. Add:

- `RequiredArray(name, description, item_schema)` — for the `actions` array
- `OptionalObject(name, description)` — for the `screenshot` config

If this is too complex for `AbpToolBuilder`, build the `browser_action` schema as raw `base::Value::Dict` directly.

## Files Changed

| File | Change |
|------|--------|
| `chrome/browser/abp/abp_http_server.cc` | Route `POST /api/v1/tabs/{id}/batch` |
| `chrome/browser/abp/abp_controller.h` | `HandleBatchRequest`, `ExecuteBatchActions` declarations |
| `chrome/browser/abp/abp_controller.cc` | Batch execution loop with 20ms inter-action delay, `NormalizeKey()`, key validation |
| `chrome/browser/abp/abp_mcp_handler.cc` | Tool definitions (33→12), dispatch table, Call* implementations, kGuideContent |
| `chrome/browser/abp/abp_mcp_handler.h` | Call* method declarations (33→12) |
| `chrome/browser/abp/abp_tool_builder.h/cc` | `RequiredArray`, `OptionalObject` (if needed) |
| `tools/abp-claude-skill/abp-browser.md` | Tool documentation update |

**No changes to**: `abp_action_context.cc/h`, `abp_input_dispatcher.cc/h`

## Verification

1. **Build**: `autoninja -C out/Default chrome`
2. **Launch**: `./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/test --no-first-run`
3. **Verify tool count**: MCP `tools/list` should show exactly 14 tools
4. **Test batch REST endpoint**:
   ```bash
   # Single action
   curl -X POST http://localhost:8222/api/v1/tabs/{id}/batch \
     -H "Content-Type: application/json" \
     -d '{"actions":[{"type":"mouse_click","x":450,"y":320}]}'

   # Multi-action: click, type, enter
   curl -X POST http://localhost:8222/api/v1/tabs/{id}/batch \
     -H "Content-Type: application/json" \
     -d '{"actions":[{"type":"mouse_click","x":450,"y":320},{"type":"keyboard_type","text":"hello"},{"type":"keyboard_press","key":"ENTER"}],"screenshot":{"markup":"interactive"}}'

   # Key normalization: abbreviations and case-insensitive
   curl -X POST http://localhost:8222/api/v1/tabs/{id}/batch \
     -H "Content-Type: application/json" \
     -d '{"actions":[{"type":"keyboard_press","key":"ctrl","modifiers":["shift"]},{"type":"keyboard_press","key":"esc"}]}'
   # → normalized to key:"CONTROL", modifiers:["SHIFT"] and key:"ESCAPE"

   # Validation error: invalid key (returns 400, nothing executed)
   curl -X POST http://localhost:8222/api/v1/tabs/{id}/batch \
     -H "Content-Type: application/json" \
     -d '{"actions":[{"type":"keyboard_press","key":"FOOBAR"}]}'

   # Validation error: coordinates outside viewport (returns 400, nothing executed)
   curl -X POST http://localhost:8222/api/v1/tabs/{id}/batch \
     -H "Content-Type: application/json" \
     -d '{"actions":[{"type":"mouse_click","x":450,"y":320},{"type":"mouse_click","x":99999,"y":99999}]}'
   ```
5. **Test MCP browser_action tool**: Via Claude Desktop, execute a click-type-submit sequence
6. **Test consolidated tools**: browser_navigate, browser_tabs, browser_dialog, browser_downloads
7. **Verify individual REST endpoints still work**: `POST /api/v1/tabs/{id}/click` etc. (unchanged)
8. **Test in Claude Desktop**: Configure MCP, verify 14 tools show up, run a browsing task
