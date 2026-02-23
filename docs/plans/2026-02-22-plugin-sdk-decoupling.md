# Plugin/SDK Decoupling Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Clean up the standalone `abp-npm` repo to contain only the Claude Code plugin (marketplace config, MCP server config, skill), removing all SDK source code that lives in the Chromium repo.

**Architecture:** The plugin repo becomes a thin Claude Code marketplace plugin. It declares an MCP server that runs `npx -y agent-browser-protocol --mcp`, which downloads and invokes the published npm package. No source code, no build step.

**Tech Stack:** Claude Code plugin system (marketplace.json, plugin.json, .mcp.json, SKILL.md)

---

### Task 1: Remove SDK Source Files

**Context:** The standalone repo at `/Users/hanwang/src/abp-npm/` currently contains the full SDK source. All SDK code lives canonically in the Chromium repo at `/Users/hanwang/src/src/tools/abp-npm/`. We need to remove the duplicate source from the plugin repo.

**Files to delete:**
- `src/` (entire directory — 12 TypeScript files + debug-ui.html)
- `scripts/` (entire directory — generate-skill.mjs)
- `tsconfig.json`
- `tsup.config.ts`
- `package.json`
- `package-lock.json`

**Step 1: Delete SDK source and build config**

```bash
cd /Users/hanwang/src/abp-npm
git rm -r src/ scripts/ tsconfig.json tsup.config.ts package.json package-lock.json
```

**Step 2: Delete untracked build artifacts**

```bash
rm -rf dist/ node_modules/ browsers/
```

**Step 3: Update .gitignore**

Replace contents of `.gitignore` with:

```
# No build artifacts in this repo
```

Since there's no build step, the old ignores (node_modules, dist, browsers, sessions, *.tgz) are unnecessary. Keep the file minimal.

**Step 4: Verify repo contents**

```bash
find . -not -path './.git/*' | sort
```

Expected remaining files:
```
.
./.claude-plugin
./.claude-plugin/marketplace.json
./.gitignore
./plugins
./plugins/agent-browser-protocol
./plugins/agent-browser-protocol/.claude-plugin
./plugins/agent-browser-protocol/.claude-plugin/plugin.json
./plugins/agent-browser-protocol/.mcp.json
./plugins/agent-browser-protocol/skills
./plugins/agent-browser-protocol/skills/abp-browser
./plugins/agent-browser-protocol/skills/abp-browser/SKILL.md
./README.md
```

**Step 5: Commit**

```bash
git add -A
git commit -m "refactor: remove SDK source, keep plugin-only files

SDK source code lives in the Chromium repo (theredsix/agent-browser-protocol).
This repo now contains only the Claude Code marketplace plugin:
marketplace config, MCP server config, and usage skill."
```

---

### Task 2: Update README

**Files:**
- Modify: `README.md`

**Step 1: Replace README with plugin-focused content**

The current README describes the full npm package. Replace it with content focused on the Claude Code plugin:

```markdown
# ABP Claude Code Plugin

Claude Code plugin for [Agent Browser Protocol](https://github.com/theredsix/agent-browser-protocol) — deterministic AI agent browser control at the engine level.

## Install

Install from the Claude Code marketplace:

```
claude plugin install theredsix/abp-npm
```

Or add manually to your project's `.mcp.json`:

```json
{
  "mcpServers": {
    "browser": {
      "command": "npx",
      "args": ["-y", "agent-browser-protocol", "--mcp"],
      "env": {
        "ABP_HEADLESS": "${ABP_HEADLESS}"
      }
    }
  }
}
```

## What's Included

- **MCP Server** — 13 browser control tools (navigate, click, type, screenshot, etc.)
- **Usage Skill** — Reference guide for all tools, loaded automatically when relevant

## Configuration

| Environment Variable | Description | Default |
|---------------------|-------------|---------|
| `ABP_HEADLESS` | Run browser headless | `""` (visible) |
| `ABP_PORT` | REST API port | `8222` |
| `ABP_BROWSER_PATH` | Custom binary path | auto-detected |

## Links

- [Agent Browser Protocol](https://github.com/theredsix/agent-browser-protocol) — Chromium fork + NPM SDK
- [NPM Package](https://www.npmjs.com/package/agent-browser-protocol) — `npm install agent-browser-protocol`
```

**Step 2: Commit**

```bash
git add README.md
git commit -m "docs: rewrite README for plugin-only repo"
```

---

### Task 3: Verify Plugin Structure

**Step 1: Validate marketplace.json**

Read `.claude-plugin/marketplace.json` and confirm:
- `name` is appropriate (currently `"abp-npm"`)
- `plugins[0].source` points to `"./plugins/agent-browser-protocol"`
- `plugins[0].version` is current
- `repository` URL is correct

**Step 2: Validate plugin.json**

Read `plugins/agent-browser-protocol/.claude-plugin/plugin.json` and confirm:
- `name` is `"agent-browser-protocol"`
- `version` matches marketplace.json
- No references to deleted files (src/, dist/, etc.)

**Step 3: Validate .mcp.json**

Read `plugins/agent-browser-protocol/.mcp.json` and confirm:
- Uses `npx -y agent-browser-protocol --mcp`
- Has `ABP_HEADLESS` env var

**Step 4: Validate SKILL.md**

Read `plugins/agent-browser-protocol/skills/abp-browser/SKILL.md` and confirm:
- Tool reference is accurate and up to date
- No references to internal SDK code

**Step 5: Fix any issues found and commit**

```bash
git add -A
git commit -m "chore: validate and fix plugin metadata"
```

(Skip this commit if no changes were needed.)

---

### Task 4: Test Plugin Installation

**Step 1: Test that the MCP server command works**

```bash
npx -y agent-browser-protocol --mcp
```

Expected: MCP proxy starts and waits for JSON-RPC on stdin. Ctrl+C to exit.

**Step 2: Verify the plugin repo can be installed in Claude Code**

```bash
claude plugin install /Users/hanwang/src/abp-npm
```

Expected: Plugin installs successfully, MCP server `browser` is registered.

**Step 3: Test the skill loads**

In a Claude Code session with the plugin installed, verify:
- The `abp-browser` skill appears in the available skills list
- The MCP tools appear when the server starts

---

### Task 5: Push and Verify

**Step 1: Review all changes**

```bash
cd /Users/hanwang/src/abp-npm
git log --oneline
git diff HEAD~3 --stat
```

**Step 2: Push to remote**

```bash
git push origin main
```

**Step 3: Verify marketplace install works from GitHub**

```bash
claude plugin install theredsix/abp-npm
```
