---
name: abp-browser
description: Use when interacting with a browser through ABP (Agent Browser Protocol) MCP tools. Guides efficient single-tool-call-per-turn browser automation.
---

# ABP Browser Control

You are connected to an ABP (Agent Browser Protocol) browser. This guide teaches you how to use the browser tools efficiently.

## Starting ABP

```bash
# Launch ABP
./out/Default/ABP.app/Contents/MacOS/ABP \
  --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run

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

### Screenshot Markup Overlays

All screenshots include visual markup overlays by default. These highlight interactive elements so you can identify click/type targets:

| Overlay | Color | What it shows |
|---------|-------|---------------|
| **clickable** | Green outline | Buttons, links, clickable elements |
| **typeable** | Orange outline | Text inputs, textareas, contenteditable |
| **scrollable** | Purple dashed outline | Scrollable containers |
| **grid** | Red 100px grid | Coordinate grid with pixel labels |
| **selected** | Blue thick outline (3px) | The currently focused element |

To disable specific overlays when they're noisy, pass `disable_markup`:

```
browser_screenshot(disable_markup: ["grid", "scrollable"])
```

With no `disable_markup`, all 5 overlays are shown.

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
| `browser_screenshot` | Screenshot + wait/observe | `disable_markup`, `format` |
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
