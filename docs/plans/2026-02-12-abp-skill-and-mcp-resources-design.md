# ABP Claude Code Skill & MCP Resources

**Date**: 2026-02-12

## Problem

ABP's MCP server exposes 30 tools but provides no guidance on how to use them effectively. Key concepts — automatic screenshots with every action, JS/virtual time pausing between actions, using `browser_screenshot` as a wait mechanism — are non-obvious and critical for efficient agent behavior. Additionally, 11 tool parameters are defined in MCP but silently ignored because the REST handlers don't implement them, which misleads clients.

## Goals

1. **Claude Code skill file**: Standalone, distributable `.md` that teaches Claude Code agents the ABP mental model and efficient tool usage patterns.
2. **MCP resources**: Standard `resources/list` + `resources/read` serving a usage guide so any MCP client can self-bootstrap.
3. **Tool description cleanup**: Remove unimplemented parameters from `GetToolDefinitions()`.

## Non-Goals

- Adding MCP prompts (deferred)
- Adding new tools (e.g., `browser_wait`) — `browser_screenshot` serves as the wait mechanism
- Changing REST API behavior

## Design

### 1. Tool Description Cleanup

Remove 11 parameters from `GetToolDefinitions()` in `abp_mcp_handler.cc`:

| Tool | Remove | Reason |
|------|--------|--------|
| `browser_new_tab` | `active`, `index` | Always foreground, always appended |
| `browser_navigate` | `referrer` | Never read by handler |
| `browser_reload` | `ignore_cache` | Always normal reload |
| `browser_type` | `delay_ms` | Hardcoded 2ms |
| `browser_screenshot` | `area`, `cursor`, `full_page` | Not implemented |
| `browser_execute_javascript` | `await_promise`, `timeout_ms` | Not passed to CDP |
| `browser_mouse_move` | `steps` | Single move, no interpolation |

Update `browser_screenshot` description to clarify it resumes execution and re-pauses (making it the "wait and observe" mechanism).

### 2. Claude Code Skill File

Location: `tools/abp-claude-skill/abp-browser.md`

Content sections:
- **Metadata**: Skill name, description, trigger conditions
- **Starting ABP**: Launch command with `--enable-abp --abp-session-dir`, readiness polling
- **Key Concepts**:
  - Every action resumes JS, performs action, waits ~500ms for settle, captures before/after screenshots, re-pauses JS
  - One tool call = one turn (screenshots are automatic, never call screenshot separately after an action)
  - `browser_screenshot` as "wait and observe" — resumes, lets page settle, captures, re-pauses
  - `markup: "interactive"` for numbered element labels with coordinates
- **Tool Quick Reference**: Compact table of all 30 tools grouped by category, verified against implementation
- **Workflow Patterns**: Navigate+interact, waiting for slow content, content extraction
- **Debugging**: Session directory layout, SQLite queries, browsing saved screenshots
- **Gotchas**: `expression` not `script`, `tab_id` defaults to active, scroll requires x/y

### 3. MCP Resources

Add to `abp_mcp_handler.h/.cc`:

- **Capability**: Add `"resources": {}` to `HandleInitialize` response
- **Routing**: Handle `resources/list` and `resources/read` in `HandleRequest`
- **Resource**: Single `abp://guide` resource containing the same core content as the skill file
- **Implementation**: Static `constexpr char[]` in the `.cc` file (no file I/O)

New methods:
```cpp
void HandleResourcesList(base::Value request_id, ResponseWithHeadersCallback callback);
void HandleResourcesRead(const base::Value::Dict& params, base::Value request_id, ResponseWithHeadersCallback callback);
```

### 4. Content Shared Between Skill and Resource

Both the Claude Code skill and the MCP resource serve the same mental model. The skill file includes Claude Code-specific metadata and formatting. The MCP resource is a markdown string returned via `resources/read`.

## Files Changed

| File | Change |
|------|--------|
| `chrome/browser/abp/abp_mcp_handler.h` | Add resource handler method declarations |
| `chrome/browser/abp/abp_mcp_handler.cc` | Remove 11 params from `GetToolDefinitions()`, add resource handlers, add guide content string |
| `tools/abp-claude-skill/abp-browser.md` | New file — Claude Code skill |
