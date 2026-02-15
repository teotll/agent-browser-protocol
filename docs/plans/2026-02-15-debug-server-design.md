# ABP Debug Server Design

A local web UI for manually controlling ABP and viewing action history with before/after screenshots. Primary use case: generating fine-tuning datasets (SQLite history + screenshots) by manually driving a headless browser.

## Architecture

Two processes on the same machine:

```
ABP Browser (port 8222)          Debug Server (port 8223)
┌──────────────────────┐         ┌──────────────────────────┐
│ REST API             │◄────────│ Proxies action requests  │
│ SQLite history DB    │         │ Reads SQLite (read-only) │
│ Screenshot files     │         │ Serves screenshot files  │
└──────────────────────┘         │ Serves web UI            │
                                 │ SSE for real-time updates│
                                 └──────────────────────────┘
                                          │
                                   Browser tab (UI)
```

- **Debug server** is a separate Node.js process (`abp-debug` CLI)
- Opens ABP's SQLite DB in **read-only mode** via `better-sqlite3`
- Reads screenshot files directly from the session directory
- Proxies action requests (click, navigate, type, etc.) to ABP's REST API
- Pushes real-time updates via SSE when `fs.watch` detects DB/screenshot changes
- Serves a single self-contained HTML file (vanilla HTML/CSS/JS, no framework)

## UI Layout

Two-panel single-page layout:

```
┌─────────────────────────────────────────────────────────────────┐
│  ABP Debug · localhost:8222 · Session: /tmp/abp-xxx · 12 actions│
├──────────────────────────┬──────────────────────────────────────┤
│  ACTION PANEL            │  HISTORY PANEL                       │
│                          │                                      │
│  Tab: [▼ select tab]     │  #12 click (x:450, y:320)    2.1s  │
│                          │  ┌─────────┐ ┌─────────┐           │
│  Action: [▼ navigate]    │  │ before  │ │ after   │           │
│                          │  └─────────┘ └─────────┘           │
│  ┌─── Parameters ─────┐  │                                      │
│  │ url: [          ]   │  │  #11 type (text:"hello")     0.8s  │
│  │                     │  │  ┌─────────┐ ┌─────────┐           │
│  └─────────────────────┘  │  │ before  │ │ after   │           │
│                          │  └─────────┘ └─────────┘           │
│  ☑ Include screenshots   │                                      │
│  [ Execute Action ]      │  #10 navigate (example.com)   1.2s  │
│                          │  ┌─────────┐ ┌─────────┐           │
│  ── Last Result ──       │  │ before  │ │ after   │           │
│  { "result": {...} }     │  └─────────┘ └─────────┘           │
├──────────────────────────┴──────────────────────────────────────┤
│  Status: Ready │ Actions: 12 │ Session dir: /tmp/abp-xxx        │
└─────────────────────────────────────────────────────────────────┘
```

### Left Panel (Action Panel)

- **Tab selector** dropdown populated from `GET /api/v1/tabs`
- **Action type** dropdown: navigate, click, type, keyPress, scroll, execute, text, wait, screenshot, back, forward, reload, move
- **Dynamic form fields** that change based on selected action
- **Include screenshots** checkbox (appends `screenshot: { area: "viewport" }` to request)
- **Execute button**
- **Last Result** section showing raw JSON response

### Right Panel (History Panel)

- Reverse-chronological list of actions read from SQLite
- Each entry: action number, type, key params summary, duration
- Before/after screenshot thumbnails per action
- Click any thumbnail for full-size modal overlay
- Auto-updates via SSE when new actions appear

## Action Form Definitions

| Action | Required Fields | Optional Fields |
|--------|----------------|-----------------|
| navigate | url (text) | referrer (text) |
| click | x (number), y (number) | button (select: left/right/middle), click_count (number), modifiers (multi-select) |
| type | text (text) | |
| keyPress | key (text) | modifiers (multi-select) |
| scroll | x (number), y (number) | delta_x (number), delta_y (number) |
| execute | script (textarea) | |
| text | | selector (text) |
| wait | ms (number) | |
| screenshot | | markup (multi-select: clickable/typeable/scrollable/grid) |
| back | | |
| forward | | |
| reload | | |
| move | x (number), y (number) | |

Modifiers multi-select renders as checkboxes: Shift, Control, Alt, Meta.

## Server Routes

```
GET  /                          → serve debug-ui.html
GET  /events                    → SSE stream (real-time updates)

# Direct data from SQLite / filesystem
GET  /data/actions              → query actions table
GET  /data/actions/:id          → query single action
GET  /data/screenshots/:file    → serve screenshot file from disk
GET  /data/session              → query session info

# Proxied to ABP REST API
GET  /api/v1/tabs               → proxy (tab list)
GET  /api/v1/browser/*          → proxy (status, session-data)
POST /api/v1/tabs/*/...         → proxy (all action requests)
DELETE /api/v1/tabs/*           → proxy (close tab)
```

## SQLite Access

Database opened read-only: `new Database(dbPath, { readonly: true })`

Key queries:

```sql
-- Session info (header bar)
SELECT id, start_time, browser_version FROM sessions
ORDER BY start_time DESC LIMIT 1

-- Action list (history panel, newest first)
SELECT id, session_id, tab_id, action_type, timestamp, duration_ms,
       params, result, success, error_message,
       screenshot_before_path, screenshot_after_path
FROM actions WHERE session_id = ? ORDER BY id DESC

-- New action detection (SSE trigger)
SELECT MAX(id) as max_id, COUNT(*) as count
FROM actions WHERE session_id = ?
```

## Real-Time Updates

1. `fs.watch` on the session directory (catches WAL changes + new screenshot files)
2. Debounce 200ms, query `MAX(id)` from actions table
3. Push SSE event with current max ID to all connected clients
4. UI compares to last known max ID, fetches new actions if changed

## File Structure

```
tools/abp-npm/
├── src/
│   ├── bin/
│   │   ├── abp.ts              (existing, unchanged)
│   │   └── debug.ts            NEW — debug server (~150 lines)
│   ├── debug-ui.html           NEW — self-contained HTML/CSS/JS
│   └── ...
├── tsup.config.ts              MODIFIED — add bin/debug entry
├── package.json                MODIFIED — add bin + better-sqlite3 dep
```

### package.json changes

```json
"bin": {
  "agent-browser-protocol": "./dist/bin/abp.mjs",
  "abp-debug": "./dist/bin/debug.mjs"
},
"dependencies": {
  "sharp": "^0.34.5",
  "better-sqlite3": "^11.0.0"
}
```

### tsup.config.ts addition

New entry: `"bin/debug": "src/bin/debug.ts"`

The HTML file is read at runtime relative to the built JS file and served on `GET /`.

## CLI

```bash
npx abp-debug [options]

Options:
  --port <port>            Debug server port (default: 8223)
  --abp-url <url>          ABP base URL (default: http://localhost:8222)
  --session-dir <path>     Path to ABP session directory (required)
  --help, -h               Show help
```

## Workflow

```bash
# Terminal 1: Launch ABP headless
npx agent-browser-protocol --headless --session-dir ./dataset/session-001

# Terminal 2: Launch debug server
npx abp-debug --session-dir ./dataset/session-001

# Open http://localhost:8223 in your browser
# Use the form UI to drive actions
# Each action generates before/after screenshots + history in SQLite
# When done, ./dataset/session-001/ is your fine-tuning dataset
```

## Dataset Output

Each action in the SQLite DB contains:

| Field | Description |
|-------|-------------|
| action_type | navigate, click, type, etc. |
| params | JSON of action parameters |
| result | JSON response from ABP |
| screenshot_before_path | viewport before the action |
| screenshot_after_path | viewport after the action |
| timestamp | Unix ms |
| duration_ms | action duration |
| success | 1 or 0 |

## Out of Scope

- Dataset export/formatting (user processes SQLite + screenshots directly)
- Annotation or labeling of actions
- Session management from the UI
- Live viewport / interactive canvas
- Authentication or multi-user support
