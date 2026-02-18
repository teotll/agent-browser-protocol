---
name: abp-browser
description: Use when interacting with a browser through ABP (Agent Browser Protocol) MCP tools. Guides efficient browser automation with batched actions.
---

# ABP Browser Control

You are connected to an ABP (Agent Browser Protocol) browser. This guide teaches you how to use the 12 browser tools efficiently.

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

1. You call a tool (e.g., `browser_action`)
2. ABP **resumes** JS execution
3. ABP dispatches your action(s)
4. ABP **waits ~500ms** for the page to settle (rendering, network, scripts)
5. ABP captures a **screenshot** automatically
6. ABP **re-pauses** JS execution
7. You receive the response with the screenshot

**One tool call = one complete turn.** You always get a screenshot back. Never call `browser_screenshot` just to see the result of an action you already performed.

### Batching Actions

`browser_action` accepts 1-3 actions per call. **Batch common workflows to reduce round-trips:**

- **Click, type, submit:** `[{mouse_click, x, y}, {keyboard_type, text}, {keyboard_press, key: ENTER}]`
- **Click and type:** `[{mouse_click, x, y}, {keyboard_type, text}]`
- **Single click:** `[{mouse_click, x, y}]`

Actions execute sequentially with a 20ms pause between each. One screenshot is taken after all actions complete.

**Do NOT batch scrolling** — use `browser_scroll` separately.

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

## Tool Reference (12 tools)

All `tab_id` parameters are optional and default to the active tab.

### Input (2 tools)
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_action` | Execute 1-3 actions (batched) | `actions` array (required). Types: mouse_click, keyboard_type, keyboard_press, mouse_hover, mouse_drag. `screenshot` config optional. |
| `browser_scroll` | Mouse wheel scroll | `x`, `y` (required), `delta_x`, `delta_y` |

**Action types for `browser_action`:**

| Type | Required | Optional |
|------|----------|----------|
| `mouse_click` | `x`, `y` | `button` (left/right/middle), `click_count`, `modifiers` |
| `keyboard_type` | `text` | - |
| `keyboard_press` | `key` | `modifiers` ([SHIFT, CONTROL, ALT, META]), `action` (press/down/up) |
| `mouse_hover` | `x`, `y` | - |
| `mouse_drag` | `start_x`, `start_y`, `end_x`, `end_y` | `steps` |

**Key names are ALL-CAPS:** ENTER, TAB, ESCAPE, BACKSPACE, DELETE, ARROWUP, ARROWDOWN, ARROWLEFT, ARROWRIGHT, HOME, END, PAGEUP, PAGEDOWN, SPACE, F1-F12, A-Z, 0-9.

**Abbreviations accepted:** CTRL→CONTROL, CMD→META, OPT→ALT, ESC→ESCAPE, DEL→DELETE, BS→BACKSPACE, CR→ENTER, PGUP→PAGEUP, PGDN→PAGEDOWN, UP→ARROWUP, DOWN→ARROWDOWN, LEFT→ARROWLEFT, RIGHT→ARROWRIGHT. macOS symbols: ⌘→META, ⌥→ALT, ⌃→CONTROL, ⇧→SHIFT.

### Navigation (2 tools)
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_navigate` | Navigate | `url` OR `action` (back, forward, reload) |
| `browser_tabs` | Tab management | `action` (list, new, close, info, activate, stop; default: list), `tab_id`, `url` |

### Observation (3 tools)
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_screenshot` | Screenshot + wait/observe | `disable_markup`, `markup`, `format` |
| `browser_javascript` | Run JS expression | `expression` (required) |
| `browser_text` | Get page text content | `selector` (optional CSS selector) |

### Situational (3 tools)
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_dialog` | Handle dialog | `action` (check, accept, dismiss; default: check), `prompt_text` |
| `browser_downloads` | Manage downloads | `action` (list, status, cancel; default: list), `download_id`, `state`, `limit` |
| `browser_files` | Provide files to file picker | `chooser_id` (required), `files`, `path`, `cancel` |

### Browser (2 tools)
| Tool | Description | Key Params |
|------|-------------|------------|
| `browser_get_status` | Check if browser is ready | - |
| `browser_shutdown` | Shut down browser | `timeout_ms` |

## Workflow Patterns

### Navigate, Click, Type, Submit (Batched)
```
1. browser_navigate(url: "https://example.com")
   → Read the screenshot to understand the page
2. browser_action(actions: [
     {type: "mouse_click", x: 150, y: 300},
     {type: "keyboard_type", text: "search query"},
     {type: "keyboard_press", key: "ENTER"}
   ])
   → All three actions execute in one turn. Read the screenshot to see results.
```

### Wait for Slow Content
```
1. browser_navigate(url: "https://slow-site.com")
   → Screenshot shows page still loading
2. browser_screenshot()
   → Still loading...
3. browser_screenshot()
   → Content has appeared, proceed
```

### Extract Structured Data
```
1. browser_navigate(url: "https://example.com/data")
2. browser_text(selector: ".results-table")
   → Returns text content of matching elements
   OR
   browser_javascript(expression: "JSON.stringify([...document.querySelectorAll('.item')].map(e => ({title: e.querySelector('h2').textContent, price: e.querySelector('.price').textContent})))")
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
- **`expression` not `script`**: The `browser_javascript` tool uses `expression` as its parameter name
- **Scroll needs coordinates**: `browser_scroll` requires `x` and `y` to specify where the mouse wheel fires — target the center of the scrollable element
- **Scroll direction**: `delta_y` positive = scroll down, negative = scroll up
- **JS is paused between actions**: Don't expect timers or animations to advance unless you perform an action
- **Key names are ALL-CAPS**: ENTER, not Enter. CONTROL, not Ctrl.
- **Never batch scrolling**: Use `browser_scroll` separately, not inside `browser_action`
