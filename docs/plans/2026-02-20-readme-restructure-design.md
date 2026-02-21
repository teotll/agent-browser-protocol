# README Restructure for v0.1.1 Release

## Goal

Restructure the ABP documentation for the v0.1.1 release to maximize adoption. Move from a monolithic README to a focused MCP-first landing page with breakout docs for REST API, MCP, compilation, and training data.

## Constraints

- GitHub repo is the only distribution channel (no published npm package, no Claude plugin)
- Pre-built binaries available via GitHub Releases
- MCP tools recently consolidated from 31 to 12
- All three platforms (macOS, Linux, Windows) can build from source

## Document Map

| File | Action | Description |
|------|--------|-------------|
| `README.md` | Rewrite | MCP-first landing page (~300-350 lines) |
| `docs/REST-API.md` | Create | Full REST API reference with its own quick start |
| `docs/MCP.md` | Create | 12-tool MCP reference with setup guides |
| `COMPILE.md` | Create | Build-from-source for macOS, Linux, Windows |
| `TRAINING.md` | Create | Session recording, SQLite schema, abp-debug, training pipelines |

Existing `plans/` directory stays as-is for internal design docs. New `docs/` files are user-facing.

---

## README.md Structure

### 1. Hero Block
- Logo/image (keep existing)
- Tagline: "Browsers are async. Agents are synchronous. ABP turns continuous browsing into discrete, atomic steps."
- One-liner: Chromium fork with embedded MCP + REST API at the engine level
- Existing sequence diagram (agent <-> ABP request/response flow)

### 2. Why Fork Chromium?
- Async vs sync mismatch pitch
- Table: what agents need vs what existing tools provide
- "Each API call is one atomic step"

### 3. Quick Start (MCP-focused)
```
Step 1: Download pre-built binary from GitHub Releases
Step 2: Start ABP (./ABP --enable-abp or platform equivalent)
Step 3: claude mcp add browser --transport streamable-http --url http://localhost:8222/mcp
Step 4: Try it - ask Claude to navigate and screenshot
```

### 4. What Makes ABP Different (7 subsections)
1. **Engine-Level Control** — HTTP server in browser process, direct access to Browser/TabStripModel/DevTools
2. **Smart Action Response** — Every action returns screenshot + events automatically
3. **Execution Control** — JS freeze between actions, virtual time, deterministic state
4. **Element Markup** — Bounding boxes on interactive/clickable/typeable elements
5. **Virtual Cursor** — Compositor-layer cursor visible in screenshots
6. **Native Event Handling** — File choosers, dialogs, downloads in event stream
7. **Session Recording for Agent Training** (NEW) — Every action logged to SQLite with before/after screenshots, successful sessions become fine-tuning data. Links to `TRAINING.md`.

### 5. Comparison Table
Keep existing ABP vs CDP/Puppeteer vs Playwright vs Selenium table. Add new row:
- "Session recording" — ABP: Built-in, all others: No

### 6. Command Line Flags
Keep existing flags table.

### 7. Project Structure
Keep abbreviated source tree listing.

### 8. Status
- Working features list
- Not yet implemented list

### 9. Testing
Brief mention + link to `TESTING.md`.

### 10. REST API
Brief mention: "ABP also exposes a full REST API for direct HTTP integration." Link to `docs/REST-API.md` for quick start and full reference.

### 11. Contributing / Maintainers / License / Acknowledgments

---

## docs/REST-API.md Structure

### 1. Overview
ABP REST API on localhost:8222.

### 2. Quick Start (REST)
- Start ABP
- `curl /api/v1/browser/status` to verify
- List tabs, create tab, navigate, click, screenshot — full curl walkthrough

### 3. Request/Response Conventions
- Base URL, content type
- Action envelope pattern (screenshot options on every action)
- Standard response shape (result, screenshots, scroll, events)

### 4. Full Endpoint Reference
All endpoint tables organized by category: Browser, Tabs, Navigation, Mouse, Keyboard, Screenshots, Content, Dialogs, Execution Control, Downloads, File Chooser, History, Wait.

### 5. Detailed Endpoint Docs
Request/response examples for each endpoint, pulled from plans/API.md.

---

## docs/MCP.md Structure

### 1. Overview
Embedded MCP server, no separate process, no Node.js. Same port as REST API.

### 2. Setup
- Claude Code: `claude mcp add browser --transport streamable-http --url http://localhost:8222/mcp`
- Claude Desktop: `claude_desktop_config.json` snippet
- Codex: equivalent config
- Generic MCP client: raw JSON-RPC examples (initialize, tools/list)

### 3. The 12 Tools
Document each consolidated tool with description, parameters, example call, example response:
- `browser_action` (click, type, press, key down/up, move, wait)
- `browser_scroll`
- `browser_navigate` (navigate, back, forward, reload)
- `browser_screenshot`
- `browser_tabs` (list, create, close, get info, activate, stop)
- `browser_javascript`
- `browser_text`
- `browser_dialog` (get, accept, dismiss)
- `browser_downloads` (list, get, cancel)
- `browser_files`
- `browser_get_status`
- `browser_shutdown`

### 4. Architecture
Embedded design diagram, benefits of single-process design.

### 5. Protocol Details
Streamable HTTP transport, protocol version, session management.

---

## COMPILE.md Structure

### 1. Prerequisites
- depot_tools setup
- Platform-specific build dependencies

### 2. Clone & Sync
- Clone the repo
- `gclient sync --no-history`
- Directory structure (src symlink)

### 3. Configure
- `gn gen` with recommended args per platform
- Debug vs release build args

### 4. Build
- `autoninja -C out/Default chrome`
- First build / incremental build time expectations

### 5. Run
- Platform-specific binary paths
- Verify with `curl /api/v1/browser/status`

### 6. Platform Notes
- macOS: signing, notarization
- Linux: headless considerations
- Windows: current status / known issues

---

## TRAINING.md Structure

### 1. Overview
- ABP records every action as a training sample: action type, parameters, before/after screenshots, result, timing, success/failure
- Sessions are self-contained datasets (SQLite DB + screenshot files)
- Vision: successful agent sessions become fine-tuning data for better models

### 2. Session Directory Layout
```
sessions/20260220_143000/
├── history.db
└── screenshots/
    ├── action_1_before.webp
    ├── action_1_after.webp
    └── ...
```

### 3. SQLite Schema
Three tables:
- `sessions` (id, start_time, end_time, browser_version, user_agent)
- `actions` (id, session_id, tab_id, action_type, timestamp, duration_ms, params, result, success, error_code, error_message, screenshot_before_path, screenshot_after_path)
- `events` (id, session_id, tab_id, event_type, timestamp, data)

### 4. Accessing Session Data
- Via REST API: GET /api/v1/history/sessions, /actions, /events, /export
- Via --abp-session-dir flag
- Direct SQLite access with example queries

### 5. Using abp-debug
- Launch ABP + abp-debug side-by-side
- Debug server UI for driving actions manually
- Viewing before/after screenshots in real time
- Generating labeled datasets by manual browsing

### 6. Training Data Pipeline
- Extracting (screenshot_before, action, screenshot_after) triples from SQLite
- Filtering for successful actions
- Mapping to VLM fine-tuning formats

---

## Key Changes from Current README

- Full API endpoint tables moved to `docs/REST-API.md`
- MCP tool list moved to `docs/MCP.md`
- Build from source moved to `COMPILE.md`
- New training data documentation in `TRAINING.md`
- Quick start refocused: GitHub Releases download -> `claude mcp add` one-liner
- Tool count updated from 31 to 12
- New section on session recording / training data generation
- Comparison table gets "Session recording" row
- REST API gets its own quick start in `docs/REST-API.md`
