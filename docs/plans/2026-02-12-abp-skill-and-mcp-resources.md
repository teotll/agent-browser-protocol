# ABP Skill File & MCP Resources Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create a Claude Code skill file and MCP resources capability so agents can self-bootstrap ABP usage.

**Architecture:** Three independent changes: (1) remove 11 unimplemented params from MCP tool definitions, (2) create a standalone Claude Code skill markdown file, (3) add `resources/list` and `resources/read` MCP methods serving a usage guide. The skill and MCP resource share the same conceptual content but are independently usable.

**Tech Stack:** C++ (Chromium), Markdown (Claude Code skill), MCP protocol (JSON-RPC 2.0)

---

### Task 1: Remove Unimplemented Parameters from Tool Definitions

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:26-244` (GetToolDefinitions)

**Step 1: Remove unimplemented params**

In `GetToolDefinitions()`, make these exact edits:

1. `browser_new_tab` (lines 38-43): Remove `.OptionalBoolean("active", ...)` and `.OptionalNumber("index", ...)`

Replace:
```cpp
  tools.Append(ToolBuilder("browser_new_tab")
                   .Description("Create a new browser tab")
                   .OptionalString("url", "URL to navigate to")
                   .OptionalBoolean("active", "Whether to activate the new tab")
                   .OptionalNumber("index", "Position in tab strip")
                   .Build());
```
With:
```cpp
  tools.Append(ToolBuilder("browser_new_tab")
                   .Description("Create a new browser tab")
                   .OptionalString("url", "URL to navigate to")
                   .Build());
```

2. `browser_navigate` (lines 56-61): Remove `.OptionalString("referrer", ...)`

Replace:
```cpp
  tools.Append(ToolBuilder("browser_navigate")
                   .Description("Navigate to a URL")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("url", "URL to navigate to")
                   .OptionalString("referrer", "Referrer URL")
                   .Build());
```
With:
```cpp
  tools.Append(ToolBuilder("browser_navigate")
                   .Description("Navigate to a URL")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("url", "URL to navigate to")
                   .Build());
```

3. `browser_reload` (lines 73-77): Remove `.OptionalBoolean("ignore_cache", ...)`

Replace:
```cpp
  tools.Append(ToolBuilder("browser_reload")
                   .Description("Reload the current page")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalBoolean("ignore_cache", "Force refresh ignoring cache")
                   .Build());
```
With:
```cpp
  tools.Append(ToolBuilder("browser_reload")
                   .Description("Reload the current page")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());
```

4. `browser_type` (lines 92-97): Remove `.OptionalNumber("delay_ms", ...)`

Replace:
```cpp
  tools.Append(ToolBuilder("browser_type")
                   .Description("Type text at current focus position")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("text", "Text to type")
                   .OptionalNumber("delay_ms", "Delay between keystrokes in ms")
                   .Build());
```
With:
```cpp
  tools.Append(ToolBuilder("browser_type")
                   .Description("Type text at current focus position")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("text", "Text to type")
                   .Build());
```

5. `browser_screenshot` (lines 99-110): Remove `area`, `cursor`, `full_page` and update description

Replace:
```cpp
  tools.Append(
      ToolBuilder("browser_screenshot")
          .Description("Take a screenshot of the page")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalString("markup",
                          "Element markup overlay: none, interactive, "
                          "clickable, typeable, inputs")
          .OptionalString("format", "Image format: png, webp, jpeg")
          .OptionalString("area", "Capture area: none, viewport")
          .OptionalBoolean("cursor", "Include virtual cursor in screenshot")
          .OptionalBoolean("full_page", "Capture full scrollable page")
          .Build());
```
With:
```cpp
  tools.Append(
      ToolBuilder("browser_screenshot")
          .Description(
              "Take a screenshot. Also acts as a wait: resumes page "
              "execution, waits for rendering to settle, captures the "
              "viewport, then re-pauses execution. Use this when you need "
              "to let the page load or update before your next action.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalString("markup",
                          "Element markup overlay: none, interactive, "
                          "clickable, typeable, inputs")
          .OptionalString("format", "Image format: png, webp, jpeg")
          .Build());
```

6. `browser_execute_javascript` (lines 112-119): Remove `await_promise` and `timeout_ms`

Replace:
```cpp
  tools.Append(
      ToolBuilder("browser_execute_javascript")
          .Description("Execute JavaScript in the page context")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("expression", "JavaScript expression to evaluate")
          .OptionalBoolean("await_promise", "Wait for promise resolution")
          .OptionalNumber("timeout_ms", "Timeout for promise resolution in ms")
          .Build());
```
With:
```cpp
  tools.Append(
      ToolBuilder("browser_execute_javascript")
          .Description("Execute JavaScript in the page context")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("expression", "JavaScript expression to evaluate")
          .Build());
```

7. `browser_mouse_move` (lines 140-146): Remove `.OptionalNumber("steps", ...)`

Replace:
```cpp
  tools.Append(ToolBuilder("browser_mouse_move")
                   .Description("Move mouse to coordinates (for hover effects)")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate")
                   .RequiredNumber("y", "Y coordinate")
                   .OptionalNumber("steps", "Intermediate steps for smooth movement")
                   .Build());
```
With:
```cpp
  tools.Append(ToolBuilder("browser_mouse_move")
                   .Description("Move mouse to coordinates (for hover effects)")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate")
                   .RequiredNumber("y", "Y coordinate")
                   .Build());
```

**Step 2: Build to verify**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles without errors

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "Remove 11 unimplemented params from MCP tool definitions"
```

---

### Task 2: Create Claude Code Skill File

**Files:**
- Create: `tools/abp-claude-skill/abp-browser.md`

**Step 1: Create the skill file**

Create `tools/abp-claude-skill/abp-browser.md` with the following content:

~~~markdown
---
name: abp-browser
description: Use when interacting with a browser through ABP (Agent Browser Protocol) MCP tools. Guides efficient single-tool-call-per-turn browser automation.
---

# ABP Browser Control

You are connected to an ABP (Agent Browser Protocol) browser. This guide teaches you how to use the browser tools efficiently.

## Starting ABP

```bash
# Launch the browser with ABP enabled
./out/Default/Chromium.app/Contents/MacOS/Chromium \
  --enable-abp --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run

# Poll until ready
while ! curl -s http://localhost:8222/api/v1/browser/status | grep '"ready":true' > /dev/null 2>&1; do sleep 1; done
echo "ABP ready"
```

Configure in Claude Desktop (`claude_desktop_config.json`):
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

## How ABP Works (Read This First)

ABP **pauses JavaScript and virtual time** between your actions. The page is frozen until your next tool call. This is critical to understand:

1. You call a tool (e.g., `browser_click`)
2. ABP **resumes** JS execution
3. ABP dispatches your action (the click)
4. ABP **waits ~500ms** for the page to settle (rendering, network, scripts)
5. ABP captures **before and after screenshots** automatically
6. ABP **re-pauses** JS execution
7. You receive the response with both screenshots

**One tool call = one complete turn.** You always get screenshots back. Never call `browser_screenshot` just to see the result of an action you already performed.

### When the Page Needs More Time

Sometimes 500ms isn't enough (page load, AJAX requests, animations). When you see the page hasn't finished loading in your screenshot:

**Call `browser_screenshot` as a "wait and observe."** It runs the same resume-wait-capture-pause cycle, giving the page another chance to settle. Repeat until the content appears.

### Using Markup Overlays

Pass `markup: "interactive"` to `browser_screenshot` (or any action that returns screenshots) to see numbered labels on all interactive elements. Each label shows the element's coordinates, making it easy to target clicks and typing.

## Tool Reference

All `tab_id` parameters are optional and default to the active tab.

### Tab Management
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_get_status` | Check if browser is ready | - |
| `browser_list_tabs` | List all open tabs | - |
| `browser_new_tab` | Open a new tab | `url` (optional) |
| `browser_close_tab` | Close a tab | `tab_id` |
| `browser_get_tab_info` | Get tab URL, title, status | `tab_id` |
| `browser_activate_tab` | Switch to a tab | `tab_id` |
| `browser_stop_loading` | Stop page load | `tab_id` |

### Navigation
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_navigate` | Go to a URL | `url` (required) |
| `browser_go_back` | Browser back | - |
| `browser_go_forward` | Browser forward | - |
| `browser_reload` | Reload page | - |

### Input
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_click` | Click at coordinates | `x`, `y` (required), `button`, `click_count`, `modifiers` |
| `browser_type` | Type text at focus | `text` (required) |
| `browser_keyboard_press` | Press key combo | `key` (required), `modifiers` |
| `browser_keyboard_down` | Hold a key down | `key` (required) |
| `browser_keyboard_up` | Release a held key | `key` (required) |
| `browser_scroll` | Mouse wheel scroll | `x`, `y` (required), `delta_x`, `delta_y` |
| `browser_mouse_move` | Move mouse (hover) | `x`, `y` (required) |

### Content
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_screenshot` | Screenshot + wait/observe | `markup`, `format` |
| `browser_execute_javascript` | Run JS expression | `expression` (required) |
| `browser_get_text` | Get page text content | `selector` (optional CSS selector) |

### Dialogs
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_get_dialog` | Check for pending dialog | - |
| `browser_accept_dialog` | Click OK on dialog | `prompt_text` (for prompt dialogs) |
| `browser_dismiss_dialog` | Click Cancel on dialog | - |

### Downloads
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_list_downloads` | List downloads | `state`, `limit` |
| `browser_get_download` | Get download info | `download_id` (required) |
| `browser_cancel_download` | Cancel download | `download_id` (required) |

### File Chooser
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_provide_files` | Provide files to file picker | `chooser_id` (required), `files`, `path`, `cancel` |

### Execution Control
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_get_execution_state` | Check if JS is paused | - |
| `browser_set_execution_state` | Pause/resume JS | `paused` (required boolean) |

### Browser
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_shutdown` | Shut down browser | `timeout_ms` |

## Workflow Patterns

### Navigate and Interact
```
1. browser_navigate(url: "https://example.com")
   → Read the after screenshot to understand the page
2. browser_click(x: 150, y: 300)
   → Read the after screenshot to see what happened
3. browser_type(text: "search query")
   → Read the after screenshot to verify input
4. browser_keyboard_press(key: "Enter")
   → Read the after screenshot to see results
```

### Wait for Slow Content
```
1. browser_navigate(url: "https://slow-site.com")
   → After screenshot shows page still loading
2. browser_screenshot()
   → Still loading...
3. browser_screenshot()
   → Content has appeared, proceed
```

### Extract Structured Data
```
1. browser_navigate(url: "https://example.com/data")
2. browser_get_text(selector: ".results-table")
   → Returns text content of matching elements
   OR
   browser_execute_javascript(expression: "JSON.stringify([...document.querySelectorAll('.item')].map(e => ({title: e.querySelector('h2').textContent, price: e.querySelector('.price').textContent})))")
   → Returns structured JSON
```

## Debugging

### Session Directory
Every ABP session stores data in a session directory:
```
sessions/<timestamp>/
├── history.db           # SQLite database with all actions and events
└── screenshots/         # Auto-saved before/after screenshots per action
```

Without `--abp-session-dir`, defaults to `/tmp/abp-<UUID>/`.

### Inspecting the SQLite Database
```bash
# Find the current session info
curl -s http://localhost:8222/api/v1/history/sessions/current | jq .

# Open the history database
sqlite3 sessions/<timestamp>/history.db

# Recent actions with status
SELECT id, type, status, url, error FROM actions ORDER BY id DESC LIMIT 10;

# Events for a specific action
SELECT * FROM events WHERE action_id = <id>;

# Screenshot file paths for an action
SELECT screenshot_before_path, screenshot_after_path FROM actions WHERE id = <id>;
```

### Browsing Saved Screenshots
Every action automatically saves before/after screenshots as WebP files in `screenshots/`. File paths are stored in the `actions` table. You can also fetch them via REST:
```bash
# Get the after screenshot for action 5
curl http://localhost:8222/api/v1/history/actions/5/screenshot?type=after -o screenshot.webp
```

## Gotchas
- **`expression` not `script`**: The `browser_execute_javascript` tool uses `expression` as its parameter name
- **Scroll needs coordinates**: `browser_scroll` requires `x` and `y` to specify where the mouse wheel fires — target the center of the scrollable element
- **Scroll direction**: `delta_y` positive = scroll down, negative = scroll up
- **JS is paused between actions**: Don't expect timers or animations to advance unless you perform an action
~~~

**Step 2: Commit**

```bash
git add tools/abp-claude-skill/abp-browser.md
git commit -m "Add Claude Code skill file for ABP browser control"
```

---

### Task 3: Add MCP Resources Support

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h:38-46` (add method declarations)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:327-345` (add routing)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:375-378` (add resources capability)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` (add guide content + handler implementations)

**Step 1: Add guide content string to anonymous namespace**

In `abp_mcp_handler.cc`, after the `GetToolDefinitions()` function closing brace (line 244) but before the `}  // namespace` closing (line 246), add:

```cpp
// Guide content served via resources/read for abp://guide
constexpr char kGuideContent[] = R"md(# ABP Browser Control Guide

## How ABP Works

ABP **pauses JavaScript and virtual time** between your actions. The page is frozen until your next tool call.

When you call any action tool (click, type, navigate, scroll, etc.):
1. ABP **resumes** JS execution
2. ABP dispatches your action
3. ABP **waits ~500ms** for the page to settle (rendering, network, scripts)
4. ABP captures **before and after screenshots** automatically
5. ABP **re-pauses** JS execution
6. You receive the response with both screenshots

**One tool call = one complete turn.** Screenshots are included automatically with every action response. There is no need to take a separate screenshot after performing an action.

## Waiting for Slow Content

Sometimes 500ms isn't enough for the page to finish loading (AJAX, animations, redirects). When the after screenshot shows incomplete content:

**Call `browser_screenshot` to wait and observe.** It runs the same resume-wait-capture-pause cycle without performing any action, giving the page another chance to settle. Repeat until the content appears.

## Markup Overlays

Pass `markup: "interactive"` to `browser_screenshot` to see numbered labels overlaid on all interactive elements. Each label shows the element's coordinates for targeting clicks and typing.

## Tool Reference

All `tab_id` parameters are optional and default to the active tab.

**Tab Management:** `browser_get_status`, `browser_list_tabs`, `browser_new_tab` (url), `browser_close_tab`, `browser_get_tab_info`, `browser_activate_tab`, `browser_stop_loading`

**Navigation:** `browser_navigate` (url required), `browser_go_back`, `browser_go_forward`, `browser_reload`

**Input:** `browser_click` (x, y required), `browser_type` (text required), `browser_keyboard_press` (key required, modifiers), `browser_keyboard_down` (key), `browser_keyboard_up` (key), `browser_scroll` (x, y required; delta_x, delta_y), `browser_mouse_move` (x, y required)

**Content:** `browser_screenshot` (markup, format), `browser_execute_javascript` (expression required), `browser_get_text` (selector)

**Dialogs:** `browser_get_dialog`, `browser_accept_dialog` (prompt_text), `browser_dismiss_dialog`

**Downloads:** `browser_list_downloads` (state, limit), `browser_get_download` (download_id), `browser_cancel_download` (download_id)

**File Chooser:** `browser_provide_files` (chooser_id required, files, path, cancel)

**Execution Control:** `browser_get_execution_state`, `browser_set_execution_state` (paused required)

**Browser:** `browser_shutdown` (timeout_ms)

## Debugging

Session data is stored in the session directory (set via `--abp-session-dir` or defaults to `/tmp/abp-<UUID>/`):

```
sessions/<timestamp>/
├── history.db           # SQLite database with sessions, actions, events
└── screenshots/         # Auto-saved before/after WebP screenshots per action
```

Query the database:
```sql
-- Recent actions
SELECT id, type, status, url, error FROM actions ORDER BY id DESC LIMIT 10;
-- Events for an action
SELECT * FROM events WHERE action_id = <id>;
-- Screenshot paths
SELECT screenshot_before_path, screenshot_after_path FROM actions WHERE id = <id>;
```

## Tips

- `browser_execute_javascript` uses `expression` as its parameter name (not `script`)
- `browser_scroll` requires `x`, `y` coordinates where the mouse wheel fires — target the element center
- Scroll direction: `delta_y` positive = scroll down, negative = scroll up
- JS is paused between actions — timers and animations don't advance until your next tool call
)md";
```

**Step 2: Add resources capability to HandleInitialize**

In `HandleInitialize` (around line 375-378), add `resources` to capabilities:

Replace:
```cpp
  base::Value::Dict capabilities;
  base::Value::Dict tools_cap;
  capabilities.Set("tools", std::move(tools_cap));
  result.Set("capabilities", std::move(capabilities));
```
With:
```cpp
  base::Value::Dict capabilities;
  base::Value::Dict tools_cap;
  capabilities.Set("tools", std::move(tools_cap));
  base::Value::Dict resources_cap;
  capabilities.Set("resources", std::move(resources_cap));
  result.Set("capabilities", std::move(capabilities));
```

**Step 3: Add routing for resources/list and resources/read**

In `HandleRequest` (around line 327-345), add two new routes before the `else` block:

Replace:
```cpp
  } else if (*rpc_method == "ping") {
    // Simple ping/pong
    base::Value::Dict result;
    SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                      std::move(callback));
  } else {
    SendJsonRpcError(std::move(request_id), kMethodNotFound, "Method not found",
                     std::move(callback));
  }
```
With:
```cpp
  } else if (*rpc_method == "resources/list") {
    HandleResourcesList(std::move(request_id), std::move(callback));
  } else if (*rpc_method == "resources/read") {
    HandleResourcesRead(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "ping") {
    // Simple ping/pong
    base::Value::Dict result;
    SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                      std::move(callback));
  } else {
    SendJsonRpcError(std::move(request_id), kMethodNotFound, "Method not found",
                     std::move(callback));
  }
```

**Step 4: Add handler method declarations to header**

In `abp_mcp_handler.h`, after line 46 (`HandleToolsCall` declaration), add:

```cpp
  void HandleResourcesList(base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void HandleResourcesRead(const base::Value::Dict& params,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
```

**Step 5: Add handler implementations**

In `abp_mcp_handler.cc`, add the two new methods right after `HandleToolsCall` (after the closing brace of `HandleToolsCall` around line 483, before `CallBrowserGetStatus`):

```cpp
void AbpMcpHandler::HandleResourcesList(base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  base::Value::Dict result;
  base::Value::List resources;

  base::Value::Dict guide;
  guide.Set("uri", "abp://guide");
  guide.Set("name", "ABP Usage Guide");
  guide.Set("description",
            "How to use ABP browser tools effectively - covers the "
            "pause/resume execution model, screenshot behavior, tool "
            "reference, and debugging");
  guide.Set("mimeType", "text/markdown");
  resources.Append(std::move(guide));

  result.Set("resources", std::move(resources));
  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleResourcesRead(const base::Value::Dict& params,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  const std::string* uri = params.FindString("uri");
  if (!uri) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing uri",
                     std::move(callback));
    return;
  }

  if (*uri != "abp://guide") {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Unknown resource: " + *uri, std::move(callback));
    return;
  }

  base::Value::Dict result;
  base::Value::List contents;

  base::Value::Dict content;
  content.Set("uri", "abp://guide");
  content.Set("mimeType", "text/markdown");
  content.Set("text", kGuideContent);
  contents.Append(std::move(content));

  result.Set("contents", std::move(contents));
  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}
```

**Step 6: Build to verify**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: Compiles without errors

**Step 7: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h chrome/browser/abp/abp_mcp_handler.cc
git commit -m "Add MCP resources capability with ABP usage guide"
```

---

### Task 4: Manual Verification

**Step 1: Start ABP and test resources/list**

```bash
# Start ABP browser
./out/Default/Chromium.app/Contents/MacOS/Chromium \
  --enable-abp --abp-session-dir=sessions/test --no-first-run &

# Wait for ready
sleep 5

# Initialize MCP session
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'
```

Expected: Response includes `"resources": {}` in capabilities.

**Step 2: Test resources/list**

```bash
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"resources/list","params":{}}' | python3 -m json.tool
```

Expected: Returns list with `abp://guide` resource.

**Step 3: Test resources/read**

```bash
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"resources/read","params":{"uri":"abp://guide"}}' | python3 -m json.tool
```

Expected: Returns guide content as markdown text.

**Step 4: Test tools/list has no unimplemented params**

```bash
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/list","params":{}}' | python3 -c "
import json, sys
data = json.load(sys.stdin)
tools = {t['name']: t for t in data['result']['tools']}

# Verify removed params are gone
assert 'active' not in json.dumps(tools['browser_new_tab'])
assert 'index' not in json.dumps(tools['browser_new_tab'])
assert 'referrer' not in json.dumps(tools['browser_navigate'])
assert 'ignore_cache' not in json.dumps(tools['browser_reload'])
assert 'delay_ms' not in json.dumps(tools['browser_type'])
assert 'area' not in json.dumps(tools['browser_screenshot'])
assert 'cursor' not in json.dumps(tools['browser_screenshot'])
assert 'full_page' not in json.dumps(tools['browser_screenshot'])
assert 'await_promise' not in json.dumps(tools['browser_execute_javascript'])
assert 'timeout_ms' not in json.dumps(tools['browser_execute_javascript'])
assert 'steps' not in json.dumps(tools['browser_mouse_move'])

# Verify screenshot description updated
assert 'wait' in tools['browser_screenshot']['description'].lower()

print('All checks passed')
"
```

Expected: "All checks passed"

**Step 5: Test unknown resource**

```bash
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":5,"method":"resources/read","params":{"uri":"abp://nonexistent"}}' | python3 -m json.tool
```

Expected: JSON-RPC error with "Unknown resource" message.
