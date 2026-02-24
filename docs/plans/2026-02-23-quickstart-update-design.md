# Quickstart Update Design

**Date**: 2026-02-23
**Status**: Approved

## Goal

Replace the existing Quick Start section in `./README.md` with an MCP-first quickstart that shows three paths: Claude Code, Codex, and HTTP mode for any MCP client.

## Current State

The existing Quick Start (README.md lines 85-122) has three options:
- Option A: Claude Code plugin marketplace install
- Option B: npm + MCP (stdio) — `claude mcp add`
- Option C: npm + MCP (streamable HTTP)

Problems: Claude-only, plugin marketplace is the primary path, Codex not mentioned, HTTP mode explanation is minimal.

## Design

Replace with three subsections in order:

### 1. Claude Code

Single command using `claude mcp add` with stdio transport:

```bash
claude mcp add browser -- npx -y agent-browser-protocol --mcp
```

Followed by a one-liner prompt example.

### 2. Codex

TOML config for `~/.codex/config.toml`:

```toml
[mcp_servers.browser]
command = "npx"
args = ["-y", "agent-browser-protocol", "--mcp"]
```

Same stdio approach, Codex's native config format.

### 3. Any MCP Client (HTTP)

Two-step: launch ABP, then connect any client to the embedded MCP endpoint.

```bash
npx -y agent-browser-protocol
```

Connect to `http://localhost:8222/mcp` via streamable-http. Show Claude Desktop JSON config as a concrete example.

### Footer Links

Keep the existing footer links but remove the docs/MCP.md reference (now covered inline) and simplify:
- npm package details → abp-npm repo
- REST API → docs/REST-API.md
- Manual binary download → MANUAL_INSTALL.md
- Building from source → COMPILE.md

## Changes

- **File**: `README.md`
- **Lines**: 85-122 (Quick Start section)
- **Action**: Replace entire section content

## Non-Goals

- No changes to the npm package README (`tools/abp-npm/README.md`)
- No changes to any code files
- Plugin marketplace install moves out of quickstart (can be referenced in npm package docs)
