# agent-browser-protocol

AI agent browser control at the engine level. A Chromium fork with a REST API for browser automation.

## Install

```bash
npm install agent-browser-protocol
```

This downloads the pre-built ABP browser binary for your platform (~130MB).

## Quick Start

```typescript
import { launch } from "agent-browser-protocol";
import fs from "node:fs";

const browser = await launch();

// Navigate
const tabs = await browser.client.tabs.list();
const tabId = tabs[0].id;
await browser.client.tabs.navigate(tabId, { url: "https://example.com" });

// Screenshot
const screenshot = await browser.client.tabs.screenshotBinary(tabId, {
  markup: "interactive",
});
fs.writeFileSync("screenshot.webp", screenshot);

// Interact
await browser.client.tabs.click(tabId, { x: 100, y: 200 });
await browser.client.tabs.type(tabId, { text: "hello world" });

// Cleanup
await browser.close();
```

## CLI

```bash
# Launch ABP
npx agent-browser-protocol

# Custom port
npx agent-browser-protocol --port 9222

# Run headless
npx agent-browser-protocol --headless

# Persist session data
npx agent-browser-protocol --session-dir ./my-session

# Pass additional Chrome flags
npx agent-browser-protocol -- --disable-gpu
```

## Claude Code Plugin

Install as a Claude Code plugin for AI-assisted browser automation:

```bash
claude plugin install agent-browser-protocol@npm
```

This gives Claude 30+ browser automation tools. The browser launches automatically on first use at 1280x800 (optimized for Claude's vision).

Screenshots are served as webp and scaled to fit Claude's vision limits. Text responses are truncated to conserve context window.

### Plugin Configuration

| Variable | Description | Default |
|----------|-------------|---------|
| `ABP_PORT` | Port for ABP server | `8222` |
| `ABP_BROWSER_PATH` | Custom binary path | auto-detected |
| `ABP_HEADLESS` | Run headless (`1`/`0`) | `0` |
| `ABP_ARGS` | Extra Chrome args (comma-separated) | none |

## Connect to Existing Instance

```typescript
import { ABPClient } from "agent-browser-protocol";

const client = new ABPClient("http://localhost:8222/api/v1");
const tabs = await client.tabs.list();
```

## API

The SDK mirrors the ABP REST API 1:1:

| SDK Method | REST Endpoint |
|-----------|--------------|
| `client.browser.status()` | `GET /browser/status` |
| `client.browser.shutdown()` | `POST /browser/shutdown` |
| `client.tabs.list()` | `GET /tabs` |
| `client.tabs.create({ url })` | `POST /tabs` |
| `client.tabs.close(id)` | `DELETE /tabs/{id}` |
| `client.tabs.navigate(id, { url })` | `POST /tabs/{id}/navigate` |
| `client.tabs.click(id, { x, y })` | `POST /tabs/{id}/click` |
| `client.tabs.type(id, { text })` | `POST /tabs/{id}/type` |
| `client.tabs.keyPress(id, { key })` | `POST /tabs/{id}/keyboard/press` |
| `client.tabs.scroll(id, { x, y, delta_y })` | `POST /tabs/{id}/scroll` |
| `client.tabs.screenshot(id)` | `POST /tabs/{id}/screenshot` |
| `client.tabs.screenshotBinary(id)` | `GET /tabs/{id}/screenshot` |
| `client.tabs.execute(id, { script })` | `POST /tabs/{id}/execute` |
| `client.tabs.text(id)` | `POST /tabs/{id}/text` |
| `client.tabs.wait(id, { ms })` | `POST /tabs/{id}/wait` |
| `client.tabs.dialog(id)` | `GET /tabs/{id}/dialog` |
| `client.tabs.dialogAccept(id)` | `POST /tabs/{id}/dialog/accept` |
| `client.tabs.dialogDismiss(id)` | `POST /tabs/{id}/dialog/dismiss` |
| `client.tabs.execution(id)` | `GET /tabs/{id}/execution` |
| `client.tabs.setExecution(id, { paused })` | `POST /tabs/{id}/execution` |
| `client.downloads.list()` | `GET /downloads` |
| `client.downloads.get(id)` | `GET /downloads/{id}` |
| `client.downloads.cancel(id)` | `POST /downloads/{id}/cancel` |
| `client.fileChooser.provide(id, opts)` | `POST /file-chooser/{id}` |

## MCP Server

ABP includes a built-in MCP server. Configure in Claude Desktop:

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
