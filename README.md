# Agent Browser Protocol

<p align="center">
  <img width="384" height="256" alt="ChatGPT Image Jan 25, 2026, 03_59_19 PM" src="https://github.com/user-attachments/assets/6cf0b584-b708-4c75-a146-dd49750e92f0" />
</p>

**Browsers are async. Agents are synchronous. ABP turns continuous browsing into discrete, atomic steps—so LLMs can reason about the web without racing against it.**

A Chromium fork with a MCP + REST API built directly into the browser engine. One request = one completed step (settled state + screenshot + event log).

```
    AI Agent                                 ABP Chromium
        │                                         │
        │  POST /click (x=450, y=320)             │
        │────────────────────────────────────────>│
        │                                         │  Inject real input event
        │                                         │  Wait for page to settle
        │                                         │  Capture compositor screenshot
        │                                         │  Collect events (e.g. tab_created)
        │                                         │  ┌─────────────────────────────┐
        │                                         │  │ PAUSE JavaScript + virtual  │
        │                                         │  │ time                        │
        │                                         │  └─────────────────────────────┘
        │  200 OK: screenshot + events            │
        │<────────────────────────────────────────│
        │                                         │
        ·  (agent inspects screenshot, decides)   ·
        │                                         │
        │  POST /type (text="Show HN")            │
        │────────────────────────────────────────>│
        │                                         │  ┌─────────────────────────────┐
        │                                         │  │ UNPAUSE JavaScript + virtual│
        │                                         │  │ time                        │
        │                                         │  └─────────────────────────────┘
        │                                         │  Inject real keyboard events
        │                                         │  Wait for page to settle
        │                                         │  Capture compositor screenshot
        │                                         │  Collect events
        │                                         │  ┌─────────────────────────────┐
        │                                         │  │ PAUSE JavaScript + virtual  │
        │                                         │  │ time                        │
        │                                         │  └─────────────────────────────┘
        │  200 OK: screenshot + events            │
        │<────────────────────────────────────────│
        │                                         │
```

No WebSocket. No CDP session management. No Puppeteer abstraction layers.
Just `curl http://localhost:8222/api/v1/tabs` and you're in.

Less than 200ms overhead per action—including screenshots. The bottleneck is the LLM, not the browser.

---

## ABP in Action

https://github.com/user-attachments/assets/739d13ac-193a-4910-b347-7493f6da15a4

Notice the freezing of the spinners while the LLM is thinking. JavaScript and virtual time are paused between actions—the page waits for the agent, not the other way around.

---

## Why Fork Chromium?

Web browsing is inherently asynchronous—events fire unpredictably, pages settle on their own timeline, state changes continuously. LLMs reason synchronously—one observation, one decision, one action. This mismatch is fundamental: existing tools force agents to race against a live browser, guessing when actions complete and papering over timing with retries.

Extensions can't fix this (sandboxed). CDP can't fix this (designed for DevTools, not autonomous control). Playwright and Puppeteer inherit the same model. We needed to go deeper.

**ABP reformats browsing into a step machine**: a request/response contract where the agent only ever acts on a stable, frozen world state.

| What agents need | What existing tools provide |
|------------------|----------------------------|
| Pause JavaScript between actions | Debugging pause |
| Pause time between actions | Real-time only |
| Compositor-layer cursor rendering | No cursor visibility |
| Simple REST API | WebSocket + session management |
| Engine-level event injection | DOM simulation or CDP passthrough |
| Action-complete detection | Manual waits or flaky heuristics |
| Event list between actions (new tab, dialog, file picker, etc.) | Polling, or async event subscriptions |

**Each API call is one atomic step.** ABP injects real input through Chromium's input system, waits for an engine-defined "settled" boundary, captures compositor output (with cursor), and returns the events that occurred. JavaScript and virtual time freeze between steps. The agent never races against the browser—it observes, decides, acts, and repeats on a world that waits for it.

---

## Quick Start

### Option A: Claude Code plugin

One command — installs the binary and auto-launches it when Claude Code needs it:

```bash
claude plugin install agent-browser-protocol@npm
```

### Option B: npm + MCP

```bash
npm install agent-browser-protocol
npx agent-browser-protocol
claude mcp add browser --transport streamable-http --url http://localhost:8222/mcp
```

Then ask Claude: *"Go to news.ycombinator.com and find the top post about AI."*

> **Using other MCP clients?** See [docs/MCP.md](docs/MCP.md) for Claude Desktop, Codex, and generic setup.
>
> **Prefer REST?** See [docs/REST-API.md](docs/REST-API.md) for curl examples and the full API reference.
>
> **Manual binary download?** See [MANUAL_INSTALL.md](MANUAL_INSTALL.md) for direct download and launch instructions.
>
> **Building from source?** See [COMPILE.md](COMPILE.md) for macOS, Linux, and Windows.

---

## What Makes ABP Different

### 1. Engine-Level Control

ABP embeds an HTTP server directly in the browser process. Requests are routed on the IO thread and dispatched on the UI thread with direct access to `Browser`, `TabStripModel`, and the DevTools agent.

```
+---------------------------------------------------------+
|                  AI Agent (curl / Python / Go)          |
+----------------------------+----------------------------+
                             | REST API
                             v
+---------------------------------------------------------+
|              AbpHttpServer (IO thread)                  |
|              localhost:8222/api/v1/*                    |
+----------------------------+----------------------------+
                             | PostTask
                             v
+---------------------------------------------------------+
|              AbpController (UI thread)                  |
|   Direct access to Browser, TabStripModel, DevTools     |
+----------------------------+----------------------------+
                             |
              +--------------+--------------+
              v              v              v
         +--------+    +----------+    +--------+
         | Input  |    | Renderer |    |Network |
         | System |    |  (Blink) |    | Stack  |
         +--------+    +----------+    +--------+
```

### 2. Smart Action Response

Every action returns what the agent needs to make the next decision:

```json
{
  "result": {"status": "clicked"},
  "screenshot_before": {
    "data": "base64-webp...",
    "width": 1920, "height": 1080
  },
  "screenshot_after": {
    "data": "base64-webp...",
    "width": 1920, "height": 1080
  },
  "scroll": {"scrollX": 0, "scrollY": 150, "pageWidth": 1280, "pageHeight": 4000, "viewportWidth": 1280, "viewportHeight": 720},
  "events": [
    {"type": "navigation", "virtual_time_ms": 0, "data": {"tab_id": "...", "url": "https://...", "frame_id": "...", "is_main_frame": true}},
    {"type": "dialog", "virtual_time_ms": 0, "data": {"tab_id": "...", "dialog_type": "confirm", "message": "Delete this item?"}},
    {"type": "file_chooser", "virtual_time_ms": 0, "data": {"id": "fc_1", "tab_id": "...", "chooser_type": "open", "multiple": false, "accepts": [".pdf", ".docx"], "pending": true}}
  ],
  "timing": {"action_started_ms": 1700000000000, "action_completed_ms": 1700000000050, "duration_ms": 50},
  "cursor": {"x": 450, "y": 320, "cursor_type": "pointer"}
}
```

No need to call "take screenshot" after every action. No need to poll for navigation events.

### 3. Execution Control

Freeze JavaScript execution between agent actions. The page stops. Timers freeze. `Date.now()` freezes. When you take a screenshot, you capture a deterministic state.

```bash
# Enable execution control
curl -X POST http://localhost:8222/api/v1/tabs/{id}/execution \
  -d '{"paused": true}'
```

Enabled by default. Disable with `--abp-disable-pause`.

### 4. Element Markup

Request bounding boxes drawn around interactive elements in any action's response screenshot:

```bash
# Markup on a click action
curl -X POST http://localhost:8222/api/v1/tabs/{id}/click \
  -d '{"x": 450, "y": 320, "screenshot": {"markup": ["clickable", "typeable"]}}'

# Markup on navigation
curl -X POST http://localhost:8222/api/v1/tabs/{id}/navigate \
  -d '{"url": "https://example.com", "screenshot": {"markup": ["typeable"]}}'
```

Markup options: `clickable`, `typeable`, `scrollable`, `grid`, `selected`.

### 5. Virtual Cursor

A compositor-layer cursor that moves with input actions and appears in screenshots. Your agent sees what a human would see.

### 6. Native Event Handling

File choosers, dialogs, and downloads are reported in the event stream:

```json
{
  "events": [
    {"type": "dialog", "data": {"tab_id": "...", "dialog_type": "confirm", "message": "Delete this item?"}}
  ]
}
```

Handle them with dedicated endpoints:

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{id}/dialog/accept
```

### 7. Session Recording for Agent Training

Every action is recorded to a SQLite database with before/after screenshots, parameters, results, timing, and success/failure status. Successful agent sessions become fine-tuning datasets for vision-language models.

```
Action #1: navigate("https://example.com")
  ├── screenshot_before.webp
  ├── params: {"url": "https://example.com"}
  └── screenshot_after.webp

Action #2: click(450, 320)
  ├── screenshot_before.webp
  ├── params: {"x": 450, "y": 320}
  └── screenshot_after.webp
```

Control session storage with `--abp-session-dir`:

```bash
./abp --abp-session-dir=./datasets/session-001
```

See [TRAINING.md](TRAINING.md) for the SQLite schema, `abp-debug` UI, and training pipeline examples.

---

## Comparison

| Feature | ABP | CDP/Puppeteer | Playwright | Selenium |
|---------|-----|---------------|------------|----------|
| REST API | Yes | No (WebSocket) | No (RPC) | Yes |
| JS execution pause | Engine-level | Debugger | No | No |
| Virtual time | Yes | Partial (CDP only) | Partial (Clock API) | No |
| Virtual cursor | Compositor | No | No | No |
| Action screenshots | Automatic | Manual | Manual | Manual |
| Event detection | Built-in | Manual subscription | Manual | Manual |
| Element markup | Built-in | No | No | No |
| Session recording | Built-in | DevTools Recorder | Codegen + Trace | Selenium IDE |
| Engine integration | Native C++ | Protocol wrapper | Protocol + browser patches | Protocol wrapper |
| Runtime.enable required | No | Yes | Yes | N/A |
| Input dispatch | Native (RenderWidgetHost) | CDP synthetic (Input.dispatch*) | CDP/Juggler synthetic | WebDriver → CDP synthetic |
| Scroll method | Native wheel events | CDP Input.dispatchMouseEvent | CDP or JS scrollIntoView | JS or Actions API |
| Compositor hit-testing | Yes (full input pipeline) | No (bypasses compositor) | No | No |
| Blocks real user input | Yes (default) | No | No | No |

---

## Command Line Flags

| Flag | Description |
|------|-------------|
| `--abp-port=8222` | API port (default: 8222) |
| `--abp-session-dir=PATH` | Session data directory (default: /tmp/abp-UUID) |
| `--abp-config=PATH` | Config file path |
| `--abp-window-size=W,H` | Window size (default: 1280,887) |
| `--abp-zoom=FACTOR` | Zoom factor (default: 1.0) |
| `--abp-disable-pause` | Disable automatic JS pause between actions |
| `--allow-system-inputs` | Allow system input (ABP blocks by default) |

---

## Project Structure

```
chrome/browser/abp/                 # Core ABP implementation
  abp_http_server.cc/h              # HTTP server (IO thread)
  abp_controller.cc/h               # Request handling (UI thread)
  abp_action_context.cc/h           # Action lifecycle (pause/resume/screenshot)
  abp_input_dispatcher.cc/h         # Native input dispatch (click/scroll/keys)
  abp_event_observer.cc/h           # CDP event client per tab
  abp_event_collector.cc/h          # Event collection during actions
  abp_mcp_handler.cc/h              # Embedded MCP server (JSON-RPC over HTTP)
  abp_tool_builder.cc/h             # MCP tool schema builder
  abp_history_controller.cc/h       # Session/action history API
  abp_history_database.cc/h         # SQLite history storage
  abp_download_observer.cc/h        # Download tracking
  abp_config.cc/h                   # Runtime configuration
  abp_types.h                       # Shared type definitions
  abp_switches.cc/h                 # Command line flags

plans/                              # Design documents
  API.md                            # REST API specification
  agent-browser-protocol.md         # Architecture
  mcp.md                            # MCP specification
```

---

## Status

ABP is under active development. Current implementation:

**Working:**
- Tab management (list, create, close, activate, stop)
- Navigation (URL, back, forward, reload)
- Screenshots with element markup and virtual cursor
- Mouse input (click, move, scroll via native wheel events)
- Keyboard input (type, press, key down/up with modifiers)
- JavaScript execution
- Text extraction (full page or CSS selector)
- Duration wait with action envelope
- Dialog handling (alert, confirm, prompt, beforeunload)
- File chooser support
- Download management
- Execution control (JS pause/resume, virtual time)
- History tracking with SQLite (sessions, actions, events)
- Virtual cursor rendering (compositor layer)
- Browser management (status, shutdown)
- MCP server with 13 tools at `/mcp`

**Not yet implemented:**
- Action success/failure tracking
- Revert URL to last known success state
- Revert browser to last known success state
- Recording of human browsing sessions as training data for agent fine-tuning

---

## Testing

ABP includes integration tests validating core functionality including navigation, input, screenshots, JavaScript execution, execution control, and MCP protocol compliance.

See [TESTING.md](TESTING.md) for the complete test matrix, test page documentation, and guide for adding new tests.

## REST API

ABP also exposes a full REST API for direct HTTP integration. See [docs/REST-API.md](docs/REST-API.md) for the quick start and complete endpoint reference.

## Maintainers
* Han Wang ([@theredsix](https://github.com/theredsix))

## Contributing

ABP is a substantial fork of Chromium. Contributions welcome, please reach out to a maintainer about contributing.

## License

Chromium is licensed under the BSD 3-Clause License. ABP modifications follow the same license.

## Acknowledgments

ABP builds on the incredible work of the Chromium team. We're grateful for their commitment to open source. This fork was created with the assistance of Claude Code.
