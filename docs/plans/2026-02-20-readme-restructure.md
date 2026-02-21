# README Restructure v0.1.1 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Restructure ABP documentation into an MCP-first README with breakout docs for REST API, MCP, compilation, and training data.

**Architecture:** Rewrite `README.md` as a focused landing page (~300-350 lines). Extract REST API reference to `docs/REST-API.md`, MCP reference to `docs/MCP.md`, build instructions to `COMPILE.md`, and training/session docs to `TRAINING.md`.

**Tech Stack:** Markdown documentation only. No code changes.

---

### Task 1: Create `docs/REST-API.md`

**Files:**
- Create: `docs/REST-API.md`

**Step 1: Write the REST API reference doc**

Create `docs/REST-API.md` with the following content structure. Pull endpoint details from the current `README.md` and `plans/API.md`.

```markdown
# ABP REST API Reference

ABP exposes a REST API on `localhost:8222` for direct HTTP control of the browser. Every action endpoint returns a standard response envelope with screenshots, events, scroll position, and timing.

## Quick Start

### 1. Start ABP

```bash
# macOS
./ABP.app/Contents/MacOS/ABP --enable-abp

# Linux
./abp --enable-abp
```

### 2. Verify

```bash
curl http://localhost:8222/api/v1/browser/status
```

### 3. Your First Session

```bash
# List open tabs
curl http://localhost:8222/api/v1/tabs

# Create a new tab
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url": "https://news.ycombinator.com"}'

# Click (replace {TAB_ID} with actual tab ID from above)
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/click \
  -H "Content-Type: application/json" \
  -d '{"x": 450, "y": 320, "screenshot": {"markup": "interactive"}}'

# Type text
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/type \
  -H "Content-Type: application/json" \
  -d '{"text": "Show HN"}'

# Take a screenshot
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/screenshot \
  -H "Content-Type: application/json" \
  -d '{"screenshot": {"markup": "interactive"}}'

# Get page text
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/text \
  -H "Content-Type: application/json" \
  -d '{}'

# Execute JavaScript
curl -X POST http://localhost:8222/api/v1/tabs/{TAB_ID}/execute \
  -H "Content-Type: application/json" \
  -d '{"script": "document.title"}'

# Close the tab
curl -X DELETE http://localhost:8222/api/v1/tabs/{TAB_ID}
```

---

## Base URL

```
http://localhost:8222/api/v1
```

All requests use `Content-Type: application/json`.

---

## Standard Response Envelope

Every action endpoint returns this structure:

```json
{
  "result": { ... },
  "screenshot_before": {
    "data": "base64-encoded-webp",
    "width": 1280,
    "height": 887,
    "virtual_time_ms": 0,
    "format": "webp"
  },
  "screenshot_after": {
    "data": "base64-encoded-webp",
    "width": 1280,
    "height": 887,
    "virtual_time_ms": 150,
    "format": "webp"
  },
  "scroll": {
    "scrollX": 0,
    "scrollY": 250,
    "pageWidth": 1280,
    "pageHeight": 4700,
    "viewportWidth": 1280,
    "viewportHeight": 887
  },
  "events": [
    {
      "type": "navigation",
      "virtual_time_ms": 100,
      "data": { "url": "https://..." }
    }
  ],
  "timing": {
    "action_started_ms": 1708444800000,
    "action_completed_ms": 1708444800150,
    "wait_completed_ms": 1708444800200,
    "duration_ms": 200
  },
  "cursor": {
    "x": 450,
    "y": 320,
    "cursor_type": "pointer"
  }
}
```

### Screenshot Options

All action endpoints accept a `screenshot` object in the request body:

```json
{
  "screenshot": {
    "markup": ["interactive"],
    "disable_markup": ["grid"],
    "format": "webp"
  }
}
```

Markup options: `clickable`, `typeable`, `scrollable`, `grid`, `selected`, `interactive` (alias for clickable + typeable).

### Event Types

| Type | Description |
|------|-------------|
| `navigation` | URL change with navigation type |
| `dialog` | alert/confirm/prompt/beforeunload dialog appeared |
| `file_chooser` | File picker dialog opened |
| `popup` | New window/tab opened |
| `tab_closed` | Tab closed with reason |
| `scroll` | Scroll position changed |
| `download_started` | Download initiated |
| `download_completed` | Download finished |

---

## Endpoints

### Browser

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/browser/status` | Check if ABP is ready |
| GET | `/browser/session-data` | Get session data file paths |
| POST | `/browser/shutdown` | Graceful shutdown |

### Tabs

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs` | List all tabs |
| GET | `/tabs/{id}` | Get tab details |
| POST | `/tabs` | Create new tab |
| DELETE | `/tabs/{id}` | Close tab |
| POST | `/tabs/{id}/activate` | Switch to tab |
| POST | `/tabs/{id}/stop` | Stop loading |

### Navigation

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/navigate` | Go to URL |
| POST | `/tabs/{id}/back` | Navigate back |
| POST | `/tabs/{id}/forward` | Navigate forward |
| POST | `/tabs/{id}/reload` | Reload page |

### Mouse Input

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/click` | Click at coordinates |
| POST | `/tabs/{id}/move` | Move mouse |
| POST | `/tabs/{id}/scroll` | Scroll (mouse wheel) |

### Keyboard Input

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/type` | Type text string |
| POST | `/tabs/{id}/keyboard/press` | Press key (with modifiers) |
| POST | `/tabs/{id}/keyboard/down` | Key down event |
| POST | `/tabs/{id}/keyboard/up` | Key up event |

### Screenshots

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs/{id}/screenshot` | Binary WebP screenshot |
| POST | `/tabs/{id}/screenshot` | Screenshot via action envelope |

### Page Content

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/execute` | Execute JavaScript |
| POST | `/tabs/{id}/text` | Get page text (full page or CSS selector) |

### Wait

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/wait` | Wait for duration (ms) |

### Dialogs

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs/{id}/dialog` | Get pending dialog info |
| POST | `/tabs/{id}/dialog/accept` | Accept dialog |
| POST | `/tabs/{id}/dialog/dismiss` | Dismiss dialog |

### Execution Control

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/tabs/{id}/execution` | Get execution state |
| POST | `/tabs/{id}/execution` | Pause/resume JavaScript + virtual time |

### Downloads

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/downloads` | List downloads |
| GET | `/downloads/{id}` | Get download status |
| POST | `/downloads/{id}/cancel` | Cancel download |

### File Chooser

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/file-chooser/{id}` | Provide files to dialog |

### History

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/history/sessions` | List sessions |
| GET | `/history/sessions/current` | Get current session |
| GET | `/history/sessions/{id}` | Get session by ID |
| GET | `/history/sessions/{id}/export` | Export session (with optional screenshots) |
| GET | `/history/actions` | List actions |
| GET | `/history/actions/{id}` | Get action by ID |
| GET | `/history/actions/{id}/screenshot` | Get action screenshot |
| DELETE | `/history/actions` | Delete actions |
| GET | `/history/events` | List events |
| GET | `/history/events/{id}` | Get event by ID |
| DELETE | `/history/events` | Delete events |
| DELETE | `/history` | Delete all history |

### Batch

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/tabs/{id}/batch` | Execute multiple actions in sequence |

---

For the full API specification with detailed request/response schemas, see [plans/API.md](../plans/API.md).
```

**Step 2: Verify all endpoint tables match the source code**

Cross-reference the endpoint tables against `abp_http_server.cc` route registrations to ensure nothing is missing.

**Step 3: Commit**

```bash
git add docs/REST-API.md
git commit -m "docs: add standalone REST API reference"
```

---

### Task 2: Create `docs/MCP.md`

**Files:**
- Create: `docs/MCP.md`

**Step 1: Write the MCP reference doc**

Create `docs/MCP.md` with the following content. Tool schemas come from `abp_mcp_handler.cc`.

```markdown
# ABP MCP Server Reference

The MCP server is embedded directly in ABP — no separate process, no Node.js, no bridge. Starting ABP with `--enable-abp` provides both the REST API and MCP protocol on the same port (8222).

## Setup

### Claude Code

```bash
claude mcp add browser --transport streamable-http --url http://localhost:8222/mcp
```

Then ask Claude: *"Go to news.ycombinator.com and find the top post about AI."*

### Claude Desktop

Add to `claude_desktop_config.json`:

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

Add to your Codex MCP configuration:

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

```bash
# Initialize session
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

# List available tools
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
```

---

## Tools (12)

### browser_action

Execute 1-3 browser input actions in a single turn. Returns one screenshot after all actions complete.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `actions` | array | Yes | Array of 1-3 action objects (see variants below) |
| `tab_id` | string | No | Target tab ID (defaults to active tab) |
| `screenshot` | object | No | Screenshot configuration (markup, format) |

**Action Variants:**

| Variant | Required Fields | Optional Fields |
|---------|----------------|-----------------|
| `mouse_click` | x, y | button, click_count, modifiers |
| `keyboard_type` | text | — |
| `keyboard_press` | key | modifiers, action |
| `mouse_hover` | x, y | — |
| `mouse_drag` | start_x, start_y, end_x, end_y | steps |

### browser_scroll

Scroll using mouse wheel at coordinates.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `x` | number | Yes | X coordinate |
| `y` | number | Yes | Y coordinate |
| `delta_x` | number | No | Horizontal scroll pixels |
| `delta_y` | number | No | Vertical scroll pixels |

### browser_navigate

Navigate to URL or go back/forward/reload.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `url` | string | No | URL to navigate to |
| `action` | string | No | "back", "forward", or "reload" |

Provide `url` to navigate, or `action` for history navigation.

### browser_screenshot

Take a screenshot of the current viewport.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `markup` | array | No | Overlays to enable: clickable, typeable, scrollable, grid, selected |
| `disable_markup` | array | No | Overlays to disable |
| `format` | string | No | "png", "webp", or "jpeg" |

Also acts as a wait — resumes page execution, waits for rendering to settle, captures viewport, then re-pauses.

### browser_tabs

Manage browser tabs.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `action` | string | No | "list" (default), "new", "close", "info", "activate", "stop" |
| `tab_id` | string | No | Target tab ID (for close/info/activate/stop) |
| `url` | string | No | URL for new tab |

### browser_javascript

Execute JavaScript in the page context.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `expression` | string | Yes | JavaScript expression to evaluate |

**Note:** Prefer `browser_action` for all user interactions. Use JavaScript only for data extraction or locating elements when actions fail.

### browser_text

Get visible text content of the page.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `selector` | string | No | CSS selector to scope text extraction |

### browser_dialog

Handle browser dialogs (alert/confirm/prompt).

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `tab_id` | string | No | Target tab ID |
| `action` | string | No | "check" (default), "accept", "dismiss" |
| `prompt_text` | string | No | Text for prompt dialogs (with accept) |

### browser_downloads

Manage downloads.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `action` | string | No | "list" (default), "status", "cancel" |
| `download_id` | string | No | Download ID (for status/cancel) |
| `state` | string | No | Filter: "in_progress", "completed", "cancelled", "failed" |
| `limit` | number | No | Max downloads to return |

### browser_files

Provide files to a pending file chooser dialog.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `chooser_id` | string | Yes | File chooser ID from event |
| `files` | array | No | File paths to provide |
| `path` | string | No | Save path for save dialogs |
| `cancel` | boolean | No | Cancel the file chooser |

### browser_get_status

Get browser status and readiness. No parameters.

### browser_shutdown

Gracefully shut down the browser.

**Parameters:**
| Name | Type | Required | Description |
|------|------|----------|-------------|
| `timeout_ms` | number | No | Timeout before force quit |

---

## Architecture

```
AI Agent (Claude Code / Codex / custom)
        │
        │  MCP Streamable HTTP (POST /mcp)
        │  or REST API (GET/POST/DELETE /api/v1/*)
        ▼
┌─────────────────────────────────────────────┐
│  AbpHttpServer (IO thread)     Port 8222    │
│  ├── /api/v1/*  → REST API                  │
│  └── /mcp       → MCP Streamable HTTP       │
├─────────────────────────────────────────────┤
│  AbpMcpHandler                              │
│  - JSON-RPC 2.0 dispatch                    │
│  - Tool name → REST action mapping          │
│  - Session management (Mcp-Session header)  │
├─────────────────────────────────────────────┤
│  AbpController (UI thread)                  │
│  - Same handler for both REST and MCP       │
│  - Direct browser engine access             │
└─────────────────────────────────────────────┘
```

Benefits of the embedded design:
- **Single process** — no Node.js, no IPC overhead
- **Single port** — REST and MCP on localhost:8222
- **Lower latency** — direct function calls, no HTTP round-trips between processes
- **Simpler deployment** — just start ABP

## Protocol Details

- **Transport:** Streamable HTTP (POST with JSON-RPC 2.0)
- **Protocol version:** 2025-03-26
- **Session management:** `Mcp-Session` header for session affinity
- **Endpoint:** `http://localhost:8222/mcp`
```

**Step 2: Verify tool names and parameters match `abp_mcp_handler.cc`**

Read through the tool dispatch section to confirm all 12 tools are documented with correct parameter names.

**Step 3: Commit**

```bash
git add docs/MCP.md
git commit -m "docs: add standalone MCP server reference"
```

---

### Task 3: Create `COMPILE.md`

**Files:**
- Create: `COMPILE.md`

**Step 1: Write the compilation guide**

Create `COMPILE.md` pulling build details from `tools/abp/build-*.sh` and the current README.

```markdown
# Building ABP from Source

ABP is a Chromium fork. Building from source follows the standard Chromium build process with ABP-specific configuration.

## Prerequisites

### All Platforms

1. **depot_tools** (Google's build toolchain):
   ```bash
   git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
   echo 'export PATH="$HOME/depot_tools:$PATH"' >> ~/.bashrc
   source ~/.bashrc
   ```

### macOS
- Xcode (latest version from App Store)
- macOS 13+ recommended

### Linux (Ubuntu/Debian)
```bash
sudo ./build/install-build-deps.sh --no-prompt
```

### Windows
- Visual Studio 2022 with C++ desktop development workload
- Windows 10/11 SDK

## Clone & Sync

```bash
# Clone the ABP repository
git clone https://github.com/anthropics/anthropic-browser-protocol.git ~/src/chromium

# Create the gclient workspace
cd ~/src
cat > .gclient << 'EOF'
solutions = [
  {
    "name": "src",
    "url": "https://github.com/anthropics/anthropic-browser-protocol.git",
    "managed": False,
    "custom_deps": {},
  },
]
EOF

# Create the src symlink (required by gclient)
ln -sf chromium src

# Sync all dependencies (this takes a while)
gclient sync --no-history
```

### Directory Structure

```
~/src/
├── .gclient
├── src -> chromium        # symlink required by gclient
└── chromium/              # ABP source code
    ├── out/               # build output
    ├── chrome/browser/abp/  # ABP implementation
    └── ...
```

## Configure

### Debug Build (faster incremental builds, recommended for development)

```bash
cd ~/src/chromium
gn gen out/Default --args='is_debug=true is_component_build=true symbol_level=1 dcheck_always_on=true'
```

### Release Build

```bash
# macOS (arm64)
gn gen out/Release-arm64 --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 target_cpu="arm64"'

# macOS (x64)
gn gen out/Release-x64 --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 target_cpu="x64"'

# Linux
gn gen out/Release --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'

# Windows
gn gen out/Release --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 enable_resource_allowlist_generation=false'
```

## Build

```bash
autoninja -C out/Default chrome    # debug
autoninja -C out/Release chrome    # release (Linux/macOS)
autoninja -C out/Release abp       # release (Windows)
```

- **First build:** 4-6 hours depending on hardware
- **Incremental builds:** seconds to minutes
- Use `autoninja` (not `ninja`) for automatic parallelism

## Run

```bash
# macOS (debug)
./out/Default/ABP.app/Contents/MacOS/ABP --enable-abp

# macOS (release)
./out/Release-arm64/ABP.app/Contents/MacOS/ABP --enable-abp

# Linux
./out/Default/abp --enable-abp
# or
./out/Release/abp --enable-abp

# Windows
.\out\Release\abp.exe --enable-abp
```

### Verify

```bash
curl http://localhost:8222/api/v1/browser/status
```

## Packaging

Release packaging scripts are in `tools/abp/`:

```bash
# macOS — creates dist/abp-{VERSION}-mac-{arch}.zip
./tools/abp/package-mac.sh [arm64|x64|universal|all]

# Linux — creates dist/abp-{VERSION}-linux-x64.tar.gz
./tools/abp/package-linux.sh
```

Version is read from `tools/abp-npm/package.json`.

## Platform Notes

### macOS
- Universal builds (arm64 + x64) are created with `chrome/installer/mac/universalizer.py`
- Signing and notarization are handled by the release scripts (`tools/abp/release-mac.sh`)

### Linux
- The packaged archive includes the binary, shared libraries, locale files, and resources
- For headless operation, ensure X11/Wayland display is available or use `--headless`

### Windows
- Build uses PowerShell (`tools/abp/build-win.ps1`)
- Resource allowlist generation is disabled for ABP builds
```

**Step 2: Commit**

```bash
git add COMPILE.md
git commit -m "docs: add build-from-source guide for all platforms"
```

---

### Task 4: Create `TRAINING.md`

**Files:**
- Create: `TRAINING.md`

**Step 1: Write the training data guide**

Create `TRAINING.md` documenting the SQLite schema, session access, abp-debug, and training pipelines.

```markdown
# Training Data with ABP

ABP automatically records every browser action as a training sample. Each action captures the complete state transition: what the page looked like before, what action was taken (with parameters), and what the page looked like after. Successful agent sessions become fine-tuning datasets for vision-language models.

## How It Works

Every API call to ABP is recorded:

```
Action #1: navigate("https://example.com")
  ├── screenshot_before.webp    (blank tab)
  ├── params: {"url": "https://example.com"}
  ├── result: {"status": "navigated"}
  └── screenshot_after.webp     (example.com loaded)

Action #2: click(450, 320)
  ├── screenshot_before.webp    (example.com, cursor at 450,320)
  ├── params: {"x": 450, "y": 320}
  ├── result: {"status": "clicked"}
  └── screenshot_after.webp     (link clicked, new page)

Action #3: type("hello world")
  ├── screenshot_before.webp    (text input focused)
  ├── params: {"text": "hello world"}
  ├── result: {"status": "typed", "length": 11}
  └── screenshot_after.webp     (text entered)
```

Each (screenshot_before, action, screenshot_after) triple is one training sample.

## Session Directory

ABP stores session data in a directory controlled by `--abp-session-dir`:

```bash
# Use a named session directory
./abp --enable-abp --abp-session-dir=./datasets/session-001

# Default: /tmp/abp-<UUID>/
./abp --enable-abp
```

### Directory Layout

```
session-001/
├── history.db                    # SQLite database
└── screenshots/
    ├── action_1_before.webp
    ├── action_1_after.webp
    ├── action_2_before.webp
    ├── action_2_after.webp
    └── ...
```

## SQLite Schema

The `history.db` file contains three tables:

### sessions

| Column | Type | Description |
|--------|------|-------------|
| id | TEXT (PK) | Session UUID |
| start_time | INTEGER | Unix timestamp (ms) |
| end_time | INTEGER | Unix timestamp (ms), 0 if active |
| browser_version | TEXT | Chrome version string |
| user_agent | TEXT | Full user agent |

### actions

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER (PK) | Auto-incrementing action ID |
| session_id | TEXT (FK) | References sessions.id |
| tab_id | TEXT | DevTools agent host ID |
| action_type | TEXT | navigate, click, type, scroll, etc. |
| timestamp | INTEGER | Unix timestamp (ms) |
| duration_ms | INTEGER | Action duration |
| params | TEXT | JSON — action parameters |
| result | TEXT | JSON — action result |
| success | INTEGER | 1 = success, 0 = failure |
| error_code | TEXT | Error code (if failed) |
| error_message | TEXT | Error message (if failed) |
| screenshot_before_path | TEXT | Path to before screenshot |
| screenshot_after_path | TEXT | Path to after screenshot |

### events

| Column | Type | Description |
|--------|------|-------------|
| id | INTEGER (PK) | Auto-incrementing event ID |
| session_id | TEXT (FK) | References sessions.id |
| tab_id | TEXT | DevTools agent host ID |
| event_type | TEXT | navigation, dialog, download_started, etc. |
| timestamp | INTEGER | Unix timestamp (ms) |
| data | TEXT | JSON — event-specific data |

## Accessing Session Data

### Via REST API

```bash
# Get current session info
curl http://localhost:8222/api/v1/history/sessions/current

# List all actions in current session
curl http://localhost:8222/api/v1/history/actions

# Get a specific action (includes params, result, screenshot paths)
curl http://localhost:8222/api/v1/history/actions/42

# Get action screenshot (returns binary WebP)
curl http://localhost:8222/api/v1/history/actions/42/screenshot -o action_42.webp

# Export full session (with optional base64 screenshots)
curl "http://localhost:8222/api/v1/history/sessions/{SESSION_ID}/export?include_screenshots=true"
```

### Direct SQLite Access

```bash
sqlite3 ./datasets/session-001/history.db
```

```sql
-- List all actions in chronological order
SELECT id, action_type, timestamp, duration_ms, success,
       screenshot_before_path, screenshot_after_path
FROM actions ORDER BY id;

-- Get successful actions only (training samples)
SELECT id, action_type, params, result,
       screenshot_before_path, screenshot_after_path
FROM actions WHERE success = 1 ORDER BY id;

-- Count actions by type
SELECT action_type, COUNT(*) as count,
       AVG(duration_ms) as avg_duration_ms
FROM actions GROUP BY action_type;

-- Get actions with their events
SELECT a.id, a.action_type, a.params,
       e.event_type, e.data
FROM actions a
LEFT JOIN events e ON a.session_id = e.session_id
  AND e.timestamp BETWEEN a.timestamp AND a.timestamp + a.duration_ms
ORDER BY a.id, e.timestamp;
```

## Using abp-debug

`abp-debug` is a local web UI for manually driving ABP and viewing action history with before/after screenshots. It's the primary tool for generating fine-tuning datasets by manual browsing.

### Launch

```bash
# Terminal 1: Start ABP with a named session
./abp --enable-abp --abp-session-dir=./datasets/session-001

# Terminal 2: Start the debug server
npx abp-debug --session-dir=./datasets/session-001
```

Open `http://localhost:8223` in your browser.

### Debug UI

The debug server provides a two-panel interface:

- **Action Panel (left):** Select a tab, choose an action type (navigate, click, type, etc.), fill in parameters, and execute. Each action is sent to ABP's REST API.
- **History Panel (right):** Reverse-chronological list of all actions with before/after screenshot thumbnails, action parameters, duration, and success/failure status. Updates in real-time via SSE.

### Architecture

```
ABP Browser (port 8222)          Debug Server (port 8223)
┌──────────────────────┐         ┌──────────────────────────┐
│ REST API             │◄────────│ Proxies action requests  │
│ SQLite history DB    │         │ Reads SQLite (read-only) │
│ Screenshot files     │         │ Serves screenshot files  │
└──────────────────────┘         │ Serves web UI            │
                                 │ SSE for real-time updates│
                                 └──────────────────────────┘
```

## Training Data Pipeline

### Extracting Training Samples

Each successful action is a training sample with the structure:

```
(observation, action, outcome)
= (screenshot_before, {action_type, params}, screenshot_after)
```

### Example: Export as JSON Lines

```python
import sqlite3
import json
import base64
from pathlib import Path

db = sqlite3.connect("./datasets/session-001/history.db")
db.row_factory = sqlite3.Row

session_dir = Path("./datasets/session-001")

samples = []
for row in db.execute(
    "SELECT * FROM actions WHERE success = 1 ORDER BY id"
):
    sample = {
        "action_type": row["action_type"],
        "params": json.loads(row["params"]) if row["params"] else {},
        "result": json.loads(row["result"]) if row["result"] else {},
        "duration_ms": row["duration_ms"],
    }

    # Load screenshots as base64
    before_path = session_dir / row["screenshot_before_path"]
    after_path = session_dir / row["screenshot_after_path"]

    if before_path.exists():
        sample["screenshot_before"] = base64.b64encode(
            before_path.read_bytes()
        ).decode()
    if after_path.exists():
        sample["screenshot_after"] = base64.b64encode(
            after_path.read_bytes()
        ).decode()

    samples.append(sample)

# Write as JSON Lines
with open("training_data.jsonl", "w") as f:
    for sample in samples:
        f.write(json.dumps(sample) + "\n")

print(f"Exported {len(samples)} training samples")
```

### Filtering and Quality

- Use `success = 1` to filter for successful actions only
- Use `duration_ms` to exclude unusually slow actions (potential errors)
- Chain actions within a session to create multi-step trajectories
- Use `events` table to identify sessions with specific behaviors (navigation, file uploads, etc.)
```

**Step 2: Commit**

```bash
git add TRAINING.md
git commit -m "docs: add training data guide with SQLite schema and abp-debug"
```

---

### Task 5: Rewrite `README.md`

**Files:**
- Modify: `README.md`

**Step 1: Rewrite the README**

Replace the entire `README.md` with the MCP-first structure. Keep the existing hero image and sequence diagram. Move API tables and build instructions to links.

The new README should follow this structure exactly:

1. **Hero** — image, tagline, one-liner, sequence diagram (keep from current)
2. **Why Fork Chromium** — keep existing pitch and needs-vs-tools table, keep "each API call is one atomic step"
3. **Quick Start** — 3 steps: download from GitHub Releases, start ABP, `claude mcp add`
4. **What Makes ABP Different** — 7 subsections:
   - Engine-Level Control (keep architecture diagram)
   - Smart Action Response (keep JSON example)
   - Execution Control (keep, trim slightly)
   - Element Markup (keep, trim slightly)
   - Virtual Cursor (keep one-liner)
   - Native Event Handling (keep JSON + curl example)
   - Session Recording for Agent Training (NEW — reference SQLite schema, link to TRAINING.md)
5. **Comparison Table** — keep existing, add "Session recording" row
6. **Command Line Flags** — updated table with all 7 flags from source
7. **Project Structure** — keep abbreviated source tree
8. **Status** — keep working/not-yet-implemented lists
9. **Testing** — brief + link to TESTING.md
10. **REST API** — brief mention + link to docs/REST-API.md
11. **Contributing / Maintainers / License / Acknowledgments** — keep existing

Key content changes:
- Quick start: Remove pre-built binary section referencing `dist/` folder. Add GitHub Releases download. Add `claude mcp add` as primary setup path.
- MCP section: Remove inline tool list, replace with brief mention linking to `docs/MCP.md`
- API section: Remove all endpoint tables, replace with brief mention linking to `docs/REST-API.md`
- Build section: Remove entirely, link to `COMPILE.md`
- Update tool count from "31" to "12" everywhere
- Add Session Recording subsection under "What Makes ABP Different"
- Add "Session recording" row to comparison table
- Update command line flags table to include all 7 flags: `--enable-abp`, `--abp-port`, `--abp-config`, `--allow-system-inputs`, `--abp-disable-pause`, `--abp-session-dir`, `--abp-window-size`, `--abp-zoom`

**Step 2: Verify all internal links resolve**

Check that these links work:
- `docs/REST-API.md`
- `docs/MCP.md`
- `COMPILE.md`
- `TRAINING.md`
- `TESTING.md`
- `plans/API.md`

**Step 3: Commit**

```bash
git add README.md
git commit -m "docs: rewrite README as MCP-first landing page for v0.1.1"
```

---

### Task 6: Update `CLAUDE.md`

**Files:**
- Modify: `CLAUDE.md`

**Step 1: Update CLAUDE.md to match new doc structure**

Update the sections in `CLAUDE.md` that reference documentation:
- Update MCP tool count from "30" to "12"
- Update any references to the README's API tables (they now live in `docs/REST-API.md`)
- Ensure the API endpoint table in CLAUDE.md stays accurate (it's a dev reference, not user-facing)

**Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md references for new doc structure"
```

---

### Task 7: Final verification

**Step 1: Check all markdown links**

```bash
# Find all internal markdown links and verify targets exist
grep -roE '\[.*?\]\(((?!http)[^)]+)\)' README.md docs/REST-API.md docs/MCP.md COMPILE.md TRAINING.md | while read line; do
  echo "Checking: $line"
done
```

**Step 2: Review file sizes**

Verify README.md is in the 300-350 line range (down from 567).

```bash
wc -l README.md docs/REST-API.md docs/MCP.md COMPILE.md TRAINING.md
```

**Step 3: Read through each doc end-to-end**

Quick read of each document to catch inconsistencies, broken formatting, or stale references.
