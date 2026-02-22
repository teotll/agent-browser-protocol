# Manual Install

For most users, we recommend installing via npm — see the [Quick Start in README.md](README.md#quick-start). This guide covers manual binary download and direct launch.

---

## 1. Download

Download a pre-built binary from the [GitHub Releases](https://github.com/theredsix/agent-browser-protocol/releases) page.

## 2. Launch

**macOS:**
```bash
./ABP.app/Contents/MacOS/ABP
```

**Linux:**
```bash
./abp
```

**Windows:**
```bash
abp.exe
```

The API starts on `localhost:8222`.

## 3. Connect

### Claude Code

```bash
claude mcp add browser --transport streamable-http --url http://localhost:8222/mcp
```

### Claude Desktop

Add to your `claude_desktop_config.json`:

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

### REST API

```bash
# Check browser readiness
curl http://localhost:8222/api/v1/browser/status

# List all tabs
curl http://localhost:8222/api/v1/tabs

# Create a new tab
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Navigate existing tab
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Click at coordinates
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":200}'

# Type text
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/type \
  -H "Content-Type: application/json" \
  -d '{"text":"hello world"}'

# Take a screenshot
curl http://localhost:8222/api/v1/tabs/{tab_id}/screenshot?markup=interactive -o screenshot.webp
```

See [docs/REST-API.md](docs/REST-API.md) for the full API reference.

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

### Session Storage

Control where session data (SQLite database, screenshots) is stored:

```bash
./abp --abp-session-dir=./datasets/session-001
```

---

## See Also

- [README.md](README.md) — npm install and project overview
- [docs/MCP.md](docs/MCP.md) — MCP server setup for Claude Code, Claude Desktop, Codex, and generic clients
- [docs/REST-API.md](docs/REST-API.md) — Full REST API reference
- [COMPILE.md](COMPILE.md) — Building from source (macOS, Linux, Windows)
