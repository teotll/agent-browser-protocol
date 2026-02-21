# agent-browser-protocol

Deterministic AI agent browser control at the engine level. A Chromium fork where every action is atomic: input, wait for settle, screenshot, pause. No race conditions.

## Install

```bash
npm install agent-browser-protocol
```

Downloads the pre-built ABP browser binary for your platform (~130MB) on first install.

## What's Inside

This package provides three things:

1. **REST API client** — typed TypeScript SDK for the 40+ endpoint ABP REST API
2. **Claude Code plugin** — 12 MCP tools + usage guide skill for AI-assisted browsing
3. **Debug server** — web UI for inspecting session history, screenshots, and action logs

---

## 1. REST API Client

Launch ABP and control it programmatically with zero dependencies beyond Node.js built-ins.

### Quick Start

```typescript
import { launch } from "agent-browser-protocol";

const browser = await launch();
const { client } = browser;

const tabs = await client.tabs.list();
const tabId = tabs[0].id;

await client.tabs.navigate(tabId, { url: "https://example.com" });
await client.tabs.click(tabId, { x: 100, y: 200 });
await client.tabs.type(tabId, { text: "hello world" });

const screenshot = await client.tabs.screenshotBinary(tabId, {
  markup: ["clickable", "typeable"],
});
fs.writeFileSync("screenshot.webp", screenshot);

await browser.close();
```

### Connect to Existing Instance

```typescript
import { ABPClient } from "agent-browser-protocol";

const client = new ABPClient("http://localhost:8222/api/v1");
const tabs = await client.tabs.list();
```

### CLI

```bash
npx agent-browser-protocol                          # launch with defaults
npx agent-browser-protocol --port 9222              # custom port
npx agent-browser-protocol --headless               # headless mode
npx agent-browser-protocol --session-dir ./session   # persist session data
npx agent-browser-protocol -- --disable-gpu          # pass Chrome flags
```

### SDK Reference

The SDK mirrors the REST API 1:1:

| SDK Method | REST Endpoint |
|-----------|--------------|
| **Browser** | |
| `client.browser.status()` | `GET /browser/status` |
| `client.browser.shutdown()` | `POST /browser/shutdown` |
| **Tabs** | |
| `client.tabs.list()` | `GET /tabs` |
| `client.tabs.create({ url })` | `POST /tabs` |
| `client.tabs.close(id)` | `DELETE /tabs/{id}` |
| `client.tabs.activate(id)` | `POST /tabs/{id}/activate` |
| `client.tabs.stop(id)` | `POST /tabs/{id}/stop` |
| **Navigation** | |
| `client.tabs.navigate(id, { url })` | `POST /tabs/{id}/navigate` |
| `client.tabs.reload(id)` | `POST /tabs/{id}/reload` |
| `client.tabs.back(id)` | `POST /tabs/{id}/back` |
| `client.tabs.forward(id)` | `POST /tabs/{id}/forward` |
| **Input** | |
| `client.tabs.click(id, { x, y })` | `POST /tabs/{id}/click` |
| `client.tabs.type(id, { text })` | `POST /tabs/{id}/type` |
| `client.tabs.keyPress(id, { key })` | `POST /tabs/{id}/keyboard/press` |
| `client.tabs.scroll(id, { x, y, delta_y })` | `POST /tabs/{id}/scroll` |
| **Observation** | |
| `client.tabs.screenshot(id)` | `POST /tabs/{id}/screenshot` |
| `client.tabs.screenshotBinary(id)` | `GET /tabs/{id}/screenshot` |
| `client.tabs.execute(id, { script })` | `POST /tabs/{id}/execute` |
| `client.tabs.text(id)` | `POST /tabs/{id}/text` |
| `client.tabs.wait(id, { ms })` | `POST /tabs/{id}/wait` |
| **Dialogs** | |
| `client.tabs.dialog(id)` | `GET /tabs/{id}/dialog` |
| `client.tabs.dialogAccept(id)` | `POST /tabs/{id}/dialog/accept` |
| `client.tabs.dialogDismiss(id)` | `POST /tabs/{id}/dialog/dismiss` |
| **Execution Control** | |
| `client.tabs.execution(id)` | `GET /tabs/{id}/execution` |
| `client.tabs.setExecution(id, { paused })` | `POST /tabs/{id}/execution` |
| **Downloads** | |
| `client.downloads.list()` | `GET /downloads` |
| `client.downloads.get(id)` | `GET /downloads/{id}` |
| `client.downloads.cancel(id)` | `POST /downloads/{id}/cancel` |
| **File Chooser** | |
| `client.fileChooser.provide(id, opts)` | `POST /file-chooser/{id}` |

---

## 2. Claude Code Plugin

Install as a Claude Code plugin for AI-assisted browser automation:

```bash
claude plugin install agent-browser-protocol@npm
```

This gives Claude:

- **12 MCP tools** — `browser_action`, `browser_scroll`, `browser_navigate`, `browser_screenshot`, `browser_tabs`, `browser_javascript`, `browser_text`, `browser_dialog`, `browser_downloads`, `browser_files`, `browser_get_status`, `browser_shutdown`
- **Usage guide skill** — automatically loaded, teaches Claude the pause/resume cycle, batching patterns, markup overlays, and debugging tips

The browser launches automatically on first tool call at 1280x800 (optimized for Claude's vision). Screenshots are served as WebP and scaled to fit Claude's vision limits. Text responses are truncated to conserve context window.

### MCP Server (Claude Desktop)

ABP also exposes a built-in MCP server for direct use in Claude Desktop:

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

### Plugin Configuration

| Variable | Description | Default |
|----------|-------------|---------|
| `ABP_PORT` | Port for ABP server | `8222` |
| `ABP_BROWSER_PATH` | Custom binary path | auto-detected |
| `ABP_HEADLESS` | Run headless (`1`/`0`) | `0` |
| `ABP_ARGS` | Extra Chrome args (comma-separated) | none |

---

## 3. Debug Server

A web UI for inspecting ABP sessions — action history, before/after screenshots, request params, errors.

```bash
npx abp-debug
```

Opens a local web server (default port 8223) that:

- **Auto-connects** to a running ABP instance and reads its session database
- **Live-updates** via SSE when new actions are recorded
- **Displays** action timeline with before/after screenshots, params, results, and errors
- **Controls** ABP lifecycle — start, stop, restart the browser from the UI
- **Proxies** API requests to ABP (so you can test endpoints from the debug UI)

### Options

```bash
npx abp-debug                                # defaults: debug on 8223, ABP on 8222
npx abp-debug --port 9223                    # custom debug server port
npx abp-debug --abp-url http://localhost:9222 # connect to ABP on different port
npx abp-debug --session-dir ./my-session      # session dir for launching ABP
npx abp-debug --abp-binary /path/to/ABP      # custom ABP binary path
```

The debug server reads ABP's SQLite session database directly (read-only) and watches for filesystem changes to push live updates.

---

## Environment Variables

| Variable | Description |
|---------|------------|
| `ABP_PORT` | Port to listen on (default: `8222`) |
| `ABP_HEADLESS=1` | Run without a visible window |
| `ABP_BROWSER_PATH` | Path to a custom ABP binary |
| `ABP_SKIP_DOWNLOAD=1` | Skip binary download during install |
| `ABP_ARGS` | Extra Chrome args, comma-separated (plugin only) |

## Platforms

- macOS (arm64, x64)
- Linux (x64)
- Windows (x64)
