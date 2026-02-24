# ABP Plugin/SDK Decoupling Design

## Problem

The Claude Code plugin (MCP proxy, skill, marketplace config) and the NPM SDK (client, launcher, installer) are co-located in the Chromium repo at `src/tools/abp-npm/`. A separate standalone repo (`theredsix/abp-npm`) exists but currently duplicates the full SDK source. This creates confusion about source of truth and couples plugin iteration to the Chromium repo.

## Goal

Decouple the Claude Code plugin from the NPM SDK so they can be developed and released independently:

- **Chromium repo** keeps the NPM SDK source and build chain
- **Plugin repo** becomes a thin Claude Code marketplace plugin that invokes the published npm package via `npx`

## Architecture

### Chromium Repo (`theredsix/agent-browser-protocol`) — No Changes

```
src/tools/abp/                    # Build/release scripts
│   └── (reads version from ../abp-npm/package.json)
src/tools/abp-npm/                # NPM SDK: agent-browser-protocol
├── src/client.ts                 # REST API client
├── src/launch.ts                 # Binary launcher
├── src/install.ts                # Binary downloader (postinstall)
├── src/mcp-proxy.ts              # MCP proxy (invoked via CLI --mcp)
├── src/transform.ts              # Response transformation (sharp)
├── src/paths.ts                  # Path + version resolution
├── src/types.ts                  # Type definitions
├── src/http.ts                   # HTTP utils
├── src/bin/abp.ts                # CLI entry point (--mcp flag)
├── src/bin/debug.ts              # Debug server
└── package.json                  # agent-browser-protocol → npm registry
```

### Plugin Repo (`theredsix/abp-npm`) — Cleaned Up

```
.claude-plugin/
└── marketplace.json              # Marketplace listing
plugins/agent-browser-protocol/
├── .claude-plugin/plugin.json    # Plugin metadata
├── .mcp.json                     # "npx -y agent-browser-protocol --mcp"
└── skills/abp-browser/SKILL.md   # Usage guide (14 tools reference)
README.md
```

## Dependency Flow

```
Claude Marketplace
  → installs plugin from theredsix/abp-npm
  → registers .mcp.json
  → on first tool use: npx -y agent-browser-protocol --mcp
  → npm downloads agent-browser-protocol (from npm registry)
  → postinstall downloads ABP binary (from GitHub Releases)
  → MCP proxy starts, proxies JSON-RPC to ABP's /mcp endpoint
```

This follows the same pattern as [Playwright MCP](https://github.com/microsoft/playwright-mcp), which uses `npx @playwright/mcp@latest` and accepts the first-run cold start (npx caches after first download).

## Versioning

| Component | Version Source | Cadence |
|-----------|---------------|---------|
| ABP binary | `src/tools/abp-npm/package.json` | Every binary release |
| NPM package | Same as binary (1:1 match) | Every binary release |
| Claude plugin | `.claude-plugin/marketplace.json` | When config/skill changes |

Plugin and npm package version independently. No cross-repo version coupling.

## Changes Required

### Plugin Repo (`theredsix/abp-npm`)

**Remove:**
- `src/` — SDK source code (stays in Chromium repo)
- `dist/` — compiled output
- `node_modules/` — dependencies
- `scripts/` — build scripts
- `browsers/` — pre-built binaries
- `package.json` — npm package definition
- `tsconfig.json`, `tsup.config.ts` — build configs
- `.gitignore` entries for build artifacts

**Keep:**
- `.claude-plugin/marketplace.json`
- `plugins/agent-browser-protocol/.claude-plugin/plugin.json`
- `plugins/agent-browser-protocol/.mcp.json`
- `plugins/agent-browser-protocol/skills/abp-browser/SKILL.md`

**Add:**
- `README.md` — explains this is the Claude Code plugin for ABP

### Chromium Repo (`theredsix/agent-browser-protocol`)

**No changes required.** The SDK source, build scripts, and `package.json` version flow all stay as-is.

## Cold Start

First invocation of `npx -y agent-browser-protocol --mcp` will:
1. Download the npm package (~seconds)
2. Run postinstall to download ABP binary (~30-60s depending on network)
3. Start the MCP proxy

Subsequent invocations use the npx cache and installed binary — near-instant startup. This is the same trade-off Playwright MCP makes.

## Future Considerations

- Pin npm version in `.mcp.json` (`agent-browser-protocol@0.1.3`) if stability matters more than getting latest
- Add more skills to the plugin repo as ABP capabilities grow
- Consider lazy binary download (defer postinstall to first tool call) to speed up initial MCP handshake
