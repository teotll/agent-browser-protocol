# Documentation Update Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Sync README.md, docs/REST-API.md, and tools/abp-npm/README.md with the current ABP implementation (18 MCP tools, new endpoints, 90.53% Mind2Web benchmark).

**Architecture:** Three independent file edits. No code changes, no tests. Each task edits one file.

**Tech Stack:** Markdown only.

**Design doc:** `docs/plans/2026-03-03-documentation-update-design.md`

---

### Task 1: Update README.md — Hero benchmark banner

**Files:**
- Modify: `README.md:7-13`

**Step 1: Add benchmark callout after tagline**

Insert after line 7 (`**Browsers are async...`), before the bullet list:

```markdown
<p align="center"><strong>90.53% on Online Mind2Web</strong> — <a href="https://github.com/theredsix/abp-online-mind2web-results">reproducible results</a></p>
```

**Step 2: Verify**

Open README.md and confirm the benchmark line renders between the tagline and the bullet list.

**Step 3: Commit**

```bash
git add README.md
git commit -m "docs: add 90.53% Online Mind2Web benchmark to README hero"
```

---

### Task 2: Update README.md — Quick Start Playwright warning

**Files:**
- Modify: `README.md` (Quick Start section, around lines 63-70)

**Step 1: Add Playwright MCP warning**

Insert a note before or after the Claude Code quick start block:

```markdown
> **Note:** If you have a Playwright MCP server configured, disable it before using ABP to avoid tool name conflicts.
```

**Step 2: Verify**

Read the Quick Start section and confirm the note is clear and well-placed.

**Step 3: Commit**

```bash
git add README.md
git commit -m "docs: add Playwright MCP conflict warning to quick start"
```

---

### Task 3: Update README.md — Status section rework

**Files:**
- Modify: `README.md:403-431` (Status section)

**Step 1: Update the status section**

Replace the current status section with:

```markdown
## Status

ABP is under active development. Current implementation:

**Working:**
- Tab management (list, create, close, activate, stop)
- Navigation (URL, back, forward, reload)
- Screenshots with element markup and virtual cursor
- Mouse input (click, move, drag, scroll via native wheel events)
- Keyboard input (type, press, key down/up with modifiers)
- JavaScript execution
- Text extraction (full page or CSS selector)
- Input helpers (slider, clear-text)
- Duration and network wait
- Dialog handling (alert, confirm, prompt, beforeunload)
- File chooser support (local files and base64 content)
- Native select popup handling
- Download management (list, status, cancel, content retrieval)
- Permission prompt handling + geolocation spoofing
- Execution control (JS pause/resume, virtual time)
- History tracking with SQLite (sessions, actions, events)
- Virtual cursor rendering (compositor layer)
- Browser management (status, shutdown)
- Embedded MCP server with 18 tools at `/mcp`

**Not yet implemented:**
- Action success/failure tracking
- Recording of human browsing sessions as training data for agent fine-tuning
```

**Step 2: Verify**

Read the updated section and confirm it reflects the current implementation accurately. Specifically check:
- MCP tool count is 18
- drag, slider, clear-text, select popup, permissions, download content are listed
- mind2web is NOT in "Not yet implemented"

**Step 3: Commit**

```bash
git add README.md
git commit -m "docs: update README status section to reflect current implementation"
```

---

### Task 4: Update docs/REST-API.md — Add missing endpoints

**Files:**
- Modify: `docs/REST-API.md`

**Step 1: Add missing endpoints to reference tables**

Add to Mouse section:
```markdown
| POST | `/tabs/{id}/drag` | Drag from start to end coordinates |
```

Add new "Input Helpers" section after Keyboard:
```markdown
### Input Helpers

| Method | Path | Description |
|--------|------|-------------|
| POST | `/tabs/{id}/slider` | Move a slider to a target value |
| POST | `/tabs/{id}/clear-text` | Clear an input field (click, select all, backspace) |
```

Add to Downloads section:
```markdown
| GET | `/downloads/{id}/content` | Get download content as base64 |
```

Add new "Popups" section after File Chooser:
```markdown
### Popups

| Method | Path | Description |
|--------|------|-------------|
| POST | `/select/{id}` | Respond to a native `<select>` dropdown |
```

Add new "Permissions" section after Popups:
```markdown
### Permissions

| Method | Path | Description |
|--------|------|-------------|
| GET | `/permissions` | List pending permission requests |
| POST | `/permissions/{id}/grant` | Grant a permission (geolocation requires lat/lng) |
| POST | `/permissions/{id}/deny` | Deny a permission |
```

**Step 2: Add missing event types**

Add to the Event Types table:
```markdown
| `file_selected` | Files were chosen or saved in a file chooser |
| `file_chooser_cancelled` | A file chooser was dismissed without selection |
| `select_open` | A native `<select>` dropdown opened (includes option list) |
| `permission_requested` | A permission prompt appeared (geolocation, camera, etc.) |
```

**Step 3: Verify**

Read the full REST-API.md and confirm:
- All new endpoints appear in correct sections
- Event types table has all 12 types
- No formatting issues

**Step 4: Commit**

```bash
git add docs/REST-API.md
git commit -m "docs: add missing endpoints and event types to REST API reference"
```

---

### Task 5: Update tools/abp-npm/README.md — MCP tools and SDK methods

**Files:**
- Modify: `tools/abp-npm/README.md`

**Step 1: Update MCP tool count and list**

Line 3: Change "17 tools" to "18 tools"

Line 19: Change "17 tools" to "18 tools"

Line 141: Update the MCP tool list to include `browser_clear_text`:
```markdown
ABP exposes 18 MCP tools: `browser_action`, `browser_scroll`, `browser_navigate`, `browser_screenshot`, `browser_tabs`, `browser_javascript`, `browser_text`, `browser_wait`, `browser_dialog`, `browser_downloads`, `browser_files`, `browser_select_picker`, `browser_get_status`, `browser_shutdown`, `browser_slider`, `browser_clear_text`, `respond_to_permission`, `set_geolocation`.
```

**Step 2: Add drag to SDK reference table**

Add to the Input section of the SDK table (after `client.tabs.move`):
```markdown
| `client.tabs.drag(id, { start_x, start_y, end_x, end_y })` | `POST /tabs/{id}/drag` |
```

The `drag` method already exists in the SDK. The following are REST/MCP-only (no SDK wrapper yet): slider, clearText, selectPicker, permissions, downloads.content.

**Step 3: Verify**

Read the updated file and confirm:
- Tool count is 18 everywhere
- `browser_clear_text` is in the tool list
- `drag` is in the SDK table
- No mention of SDK methods that don't exist

**Step 4: Commit**

```bash
git add tools/abp-npm/README.md
git commit -m "docs: update npm README with 18 MCP tools and drag SDK method"
```

---

### Task 6: Final review commit

**Step 1: Review all changes**

```bash
git diff HEAD~5..HEAD --stat
```

Verify all 3 files were modified. Read through each to confirm consistency:
- README.md mentions 18 MCP tools in both hero and status
- REST-API.md has all endpoints matching the implementation
- npm README has 18 tools and correct SDK table

**Step 2: No additional commit needed if all prior commits are clean**
