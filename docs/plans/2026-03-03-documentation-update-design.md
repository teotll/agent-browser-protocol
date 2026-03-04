# Documentation Update Design — 2026-03-03

Sync README.md, docs/REST-API.md, and tools/abp-npm/README.md with the current ABP implementation.

## Motivation

Documentation lags the implementation: MCP tool count is wrong (14/17 vs actual 18), several REST endpoints are undocumented, event types are missing, and the 90.53% Online Mind2Web benchmark result is not mentioned.

## Changes

### 1. README.md

**Hero benchmark banner** — Add immediately after the tagline (line 7):
- "90.53% on Online Mind2Web" with link to https://github.com/theredsix/abp-online-mind2web-results
- Blockquote or bold callout style, matching the existing hero section aesthetic

**Quick Start section** — Add note recommending users disable the Playwright MCP server before trying ABP to avoid tool name confusion.

**Status section rework:**
- Update MCP tool count: 14 → 18
- Add to "Working" list: native select popup handling, permission prompts + geo-spoofing, drag, slider input, clear-text, download content retrieval (base64), network wait
- Remove "Online mind2web benchmarks" from "Not yet implemented"
- Clean up remaining "Not yet implemented" items to only genuinely unimplemented features

### 2. docs/REST-API.md

**Add missing endpoints:**
- Mouse: `POST /tabs/{id}/drag`
- Input helpers (new section): `POST /tabs/{id}/slider`, `POST /tabs/{id}/clear-text`
- Downloads: `GET /downloads/{id}/content`
- Popups (new section): `POST /select/{id}`
- Permissions (new section): `GET /permissions`, `POST /permissions/{id}/grant`, `POST /permissions/{id}/deny`

**Add missing event types:**
- `file_selected`, `file_chooser_cancelled`, `select_open`, `permission_requested`

### 3. tools/abp-npm/README.md

**MCP tool count:** 17 → 18

**MCP tool list:** Add `browser_clear_text`

**SDK reference table — add missing methods:**
- `client.tabs.drag(id, opts)` → `POST /tabs/{id}/drag`
- `client.tabs.slider(id, opts)` → `POST /tabs/{id}/slider`
- `client.tabs.clearText(id, opts)` → `POST /tabs/{id}/clear-text`
- `client.downloads.content(id)` → `GET /downloads/{id}/content`
- `client.selectPicker.respond(id, opts)` → `POST /select/{id}`
- `client.permissions.list()` → `GET /permissions`
- `client.permissions.grant(id, opts)` → `POST /permissions/{id}/grant`
- `client.permissions.deny(id, opts)` → `POST /permissions/{id}/deny`

Note: Verify actual SDK method names from npm package source before writing.

## Out of Scope

- plans/API.md and plans/mcp.md (internal design docs, not user-facing)
- COMPILE.md, MANUAL_INSTALL.md, TRAINING.md, TESTING.md (no relevant changes needed)
- Adding new documentation files
