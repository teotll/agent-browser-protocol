---
name: abp-browser
description: Use when controlling a browser via ABP (Agent Browser Protocol). Provides tool usage guide, best practices, and debugging tips for the 12 MCP browser tools.
---

# ABP Browser Control Guide

## How ABP Works

ABP **pauses JavaScript and virtual time** between your actions. The page is frozen until your next tool call.

When you call any action tool (browser_action, browser_scroll, browser_navigate, etc.):
1. ABP **resumes** JS execution
2. ABP dispatches your action(s)
3. ABP **waits for the page to settle** (three-phase: 150ms for JS to fire handlers → tracks triggered network requests until they complete or 1s timeout → 150ms DOM settle)
4. ABP captures a **screenshot** automatically
5. ABP **re-pauses** JS execution
6. You receive the response with the screenshot

**One tool call = one complete turn.** Screenshots are included automatically with every action response. There is no need to take a separate screenshot after performing an action.

## Batching Actions

`browser_action` accepts 1-3 actions per call. Batch common workflows to reduce round-trips:

- **Click, type, submit:** `[{mouse_click, x, y}, {keyboard_type, text}, {keyboard_press, key: ENTER}]`
- **Click and type:** `[{mouse_click, x, y}, {keyboard_type, text}]`
- **Single click:** `[{mouse_click, x, y}]`

Actions execute sequentially with a 20ms pause between each. One screenshot is taken after all actions complete.

**Do NOT batch scrolling** — use `browser_scroll` separately.

## Waiting for Slow Content

ABP automatically tracks network requests triggered by your action and waits for them to complete (up to 1s for clicks, 60s for file uploads). If requests don't finish in time, a `request_tracking_timeout` event appears in the response — the screenshot may not reflect the final page state.

When the screenshot shows incomplete content, **call `browser_screenshot`** to wait and observe. It runs the same resume-wait-capture-pause cycle without performing any action, giving the page another chance to settle. Repeat until the content appears.

## Markup Overlays

Pass `markup: ["clickable", "typeable", "grid"]` to `browser_screenshot` to see labeled overlays on interactive elements. Each label shows the element's coordinates for targeting clicks and typing.

## Tool Reference (16 tools)

All `tab_id` parameters are optional and default to the active tab.

**Input:**
- `browser_action` — 1-3 actions: mouse_click (x, y), keyboard_type (text), keyboard_press (key, modifiers?), mouse_hover (x, y), mouse_drag (start_x, start_y, end_x, end_y). Keys are ALL-CAPS (ENTER, TAB, ESCAPE, CONTROL, META, etc.). Abbreviations accepted: CTRL, CMD, ESC, DEL.
- `browser_scroll` — x, y (where wheel fires), delta_x?, delta_y? (positive=down/right)
- `browser_slider` — orientation (horizontal/vertical), track bounds, current position, min, max, target_value. Calculates and executes drag automatically. Fallback chain if result is wrong: (1) `browser_action` with `mouse_drag`, (2) click the slider then use ARROWRIGHT/ARROWLEFT (or ARROWUP/ARROWDOWN) to nudge incrementally.

**Navigation:**
- `browser_navigate` — url? OR action? (back, forward, reload)
- `browser_tabs` — action? (list, new, close, info, activate, stop; default: list), tab_id?, url?

**Observation:**
- `browser_screenshot` — markup?, disable_markup?, format?
- `browser_javascript` — expression (required). Data extraction and DOM inspection ONLY — do NOT use for interaction; prefer browser_action.
- `browser_text` — selector?

**Situational:**
- `browser_dialog` — action? (check, accept, dismiss; default: check), prompt_text?
- `browser_downloads` — action? (list, status, cancel, content; default: list), download_id?, state?, limit?, max_size?. Use action:"content" with download_id to retrieve file bytes as base64 BlobResourceContents.
- `browser_files` — chooser_id (required), files?, content_files?, path?, cancel?, max_size?. Use content_files for base64 uploads: [{filename, data, mime_type}].
- `browser_select_picker` — popup_id (required), indices? (array of ints), cancel?. Respond to a pending <select> popup.
- `respond_to_permission` — permission_id (required), permission_type (required), allow (required), latitude?, longitude?, accuracy?. Respond to a permission prompt. When granting geolocation, provide latitude and longitude for mock coordinates.

**Browser:**
- `browser_get_status` — no params
- `browser_shutdown` — timeout_ms?

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

## Best Practices

- **Use filters and sorting aggressively.** When a page offers filters (price range, category, date, ratings, size, color, etc.), sorting options, or faceted search — always apply them to narrow results before scrolling through content. This reduces the number of pages you need to process and gets to relevant results faster.
- **Prefer search over browsing.** If you know what you're looking for, use the site's search bar rather than clicking through menus.

## Tips

- `browser_javascript` uses `expression` as its parameter name (not `script`)
- `browser_scroll` requires `x`, `y` coordinates where the mouse wheel fires — target the element center
- Scroll direction: `delta_y` positive = scroll down, negative = scroll up
- JS is paused between actions — timers and animations don't advance until your next tool call
- Key names are ALL-CAPS: ENTER, TAB, ESCAPE, BACKSPACE, ARROWUP, ARROWDOWN, etc.
- Modifier keys for keyboard_press: SHIFT, CONTROL, ALT, META (or abbreviations CTRL, CMD, OPT)
