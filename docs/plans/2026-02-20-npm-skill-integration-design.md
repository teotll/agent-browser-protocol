# ABP npm Package — Skill Integration & Binary Publish Design

## Overview

Finalize the `tools/abp-npm/` package so that:
1. `npm install agent-browser-protocol` downloads pre-built ABP binaries from GitHub Releases via postinstall
2. `claude plugin install agent-browser-protocol@npm` installs a Claude skill teaching effective ABP MCP tool usage

The npm package already exists with binary download, TypeScript SDK, CLI, MCP proxy, and Claude plugin metadata. This design adds the missing skill and verifies the binary download path works with published releases.

## What Exists

```
tools/abp-npm/
├── package.json              # v0.1.1, postinstall: node dist/install.js
├── .claude-plugin/plugin.json # Plugin manifest
├── .mcp.json                 # MCP server config (stdio proxy)
├── src/
│   ├── index.ts              # Public exports (SDK)
│   ├── install.ts            # postinstall: download from GitHub Releases
│   ├── paths.ts              # Platform detection, version constants
│   ├── launch.ts             # Spawn ABP process, wait for ready
│   ├── client.ts             # Typed REST API wrapper
│   ├── http.ts               # Internal HTTP helper
│   ├── mcp-proxy.ts          # stdio→HTTP MCP proxy for Claude Code
│   ├── transform.ts          # Image scaling (sharp), text truncation
│   ├── types.ts              # TypeScript types
│   └── bin/abp.ts            # CLI entry point
├── dist/                     # Built output
└── browsers/                 # Downloaded binaries (gitignored)
```

## What's New

### 1. Skill Generation Script

`scripts/generate-skill.mjs` — extracts `kGuideContent` from the C++ MCP handler and generates a SKILL.md file.

**Source:** `chrome/browser/abp/abp_mcp_handler.cc` — the `kGuideContent` raw string (between `R"md(` and `)md"` delimiters).

**Output:** `skills/abp-browser/SKILL.md` with YAML frontmatter prepended.

**Frontmatter:**
```yaml
---
name: abp-browser
description: Use when controlling a browser via ABP (Agent Browser Protocol). Provides tool usage guide, best practices, and debugging tips.
---
```

**Error handling:** If the C++ file can't be found (building outside Chromium tree), warn to stderr but don't fail. The existing SKILL.md (committed to git) serves as fallback.

### 2. Package Structure Changes

Add to `tools/abp-npm/`:
```
├── scripts/
│   └── generate-skill.mjs    # kGuideContent → SKILL.md
└── skills/
    └── abp-browser/
        └── SKILL.md           # Generated, committed for npm publish
```

### 3. package.json Changes

```diff
  "files": [
    "dist/",
    "browsers/",
    ".claude-plugin/",
    ".mcp.json",
+   "skills/",
    "src/debug-ui.html"
  ],
  "scripts": {
    "build": "tsup",
+   "generate-skill": "node scripts/generate-skill.mjs",
+   "prebuild": "node scripts/generate-skill.mjs",
    "postinstall": "node dist/install.js",
    "typecheck": "tsc --noEmit"
  },
```

## User Flow

### SDK + Binary
```bash
npm install agent-browser-protocol
# postinstall downloads ~130MB binary from GitHub Releases
# Binary extracted to node_modules/agent-browser-protocol/browsers/
```

### Claude Plugin + Skill
```bash
claude plugin install agent-browser-protocol@npm
# Claude discovers:
#   .claude-plugin/plugin.json → plugin identity
#   .mcp.json → MCP server (browser tools)
#   skills/abp-browser/SKILL.md → usage guide
```

### Programmatic
```typescript
import { launch } from "agent-browser-protocol";
const browser = await launch();
// ... use browser.client
await browser.close();
```

## Binary Download Path

Already implemented in `install.ts`. The flow:
1. Detect platform: `darwin-arm64`, `darwin-x64`, `linux-x64`, `win32-x64`
2. Construct URL: `https://github.com/anthropics/anthropic-browser/releases/download/v{version}/abp-{version}-chrome-{chromeVersion}-{platform}-{arch}.{zip|tar.gz}`
3. Download and extract to `browsers/`
4. Verify executable exists

Environment overrides:
- `ABP_SKIP_DOWNLOAD=1` — skip download
- `ABP_BROWSER_PATH` — use custom binary path

## Platforms

- macOS arm64, x64
- Linux x64
- Windows x64
