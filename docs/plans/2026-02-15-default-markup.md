# Default Markup Overlays Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make all 5 markup overlays (clickable, typeable, scrollable, grid, selected) enabled by default on screenshots, and switch the API parameter from `markup` (opt-in) to `disable_markup` (opt-out).

**Architecture:** Add a `kAllMarkupTags` constant listing all 5 tags. Replace `markup` parameter parsing with `disable_markup` parsing everywhere (C++ REST, MCP, TypeScript SDK). Compute effective tags as `all - disabled`. Add `selected` CSS rule targeting `document.activeElement` via `:focus` with a thicker 3px blue border.

**Tech Stack:** C++ (Chromium), TypeScript (npm SDK), HTML/JS (debug UI)

**Design doc:** `docs/plans/2026-02-15-default-markup-design.md`

---

### Task 1: Add `selected` Tag and `kAllMarkupTags` Constant

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:194-205`
- Modify: `chrome/browser/abp/abp_controller.cc:366-380`

**Step 1: Add `kAllMarkupTags` constant to header**

In `chrome/browser/abp/abp_controller.h`, add before the `AbpController` class (around line 50, near other constants):

```cpp
// All available markup tags for screenshot overlays.
// These are all enabled by default; use disable_markup to turn off specific ones.
inline constexpr const char* kAllMarkupTags[] = {
    "clickable", "typeable", "scrollable", "grid", "selected"};
inline constexpr size_t kAllMarkupTagsCount = 5;
```

Also add a helper function declaration near the other static helpers in the `AbpController` class:

```cpp
static std::vector<std::string> ComputeEffectiveMarkupTags(
    const std::vector<std::string>& disable_tags);
```

**Step 2: Update `IsValidMarkupTag` to include `selected`**

In `chrome/browser/abp/abp_controller.cc:366-369`, change:

```cpp
bool IsValidMarkupTag(const std::string& tag) {
  return tag == "clickable" || tag == "typeable" ||
         tag == "scrollable" || tag == "grid" || tag == "selected";
}
```

**Step 3: Add `ComputeEffectiveMarkupTags` implementation**

Add after `ValidateMarkupTags` (after line 380):

```cpp
// static
std::vector<std::string> AbpController::ComputeEffectiveMarkupTags(
    const std::vector<std::string>& disable_tags) {
  std::vector<std::string> effective;
  for (const char* tag : kAllMarkupTags) {
    if (std::find(disable_tags.begin(), disable_tags.end(), tag) ==
        disable_tags.end()) {
      effective.emplace_back(tag);
    }
  }
  return effective;
}
```

**Step 4: Build to verify compilation**

Run: `autoninja -C out/Default chrome`
Expected: Compiles without errors.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat: add selected markup tag and kAllMarkupTags constant"
```

---

### Task 2: Add `selected` CSS to Markup Injection

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:886-977` (BuildMarkupInjectionScript)
- Modify: `chrome/browser/abp/abp_controller.cc:980-995` (BuildMarkupCleanupScript)

**Step 1: Add `selected` CSS rule to BuildMarkupInjectionScript**

In `chrome/browser/abp/abp_controller.cc`, in the `BuildMarkupInjectionScript` function, find where `has_scrollable` and `has_grid` booleans are computed (around line 909-912). Add:

```cpp
bool has_selected = std::find(markup_tags.begin(), markup_tags.end(),
                              "selected") != markup_tags.end();
```

Then add the CSS rule for `selected` alongside the other CSS rules (after the typeable rule, before the scrollable detection JS):

```cpp
if (has_selected) {
  css_rules += R"(
    *:focus{outline:3px solid #2196F3!important;outline-offset:-3px!important}
  )";
}
```

Note: The `*:focus` selector highlights `document.activeElement`. The 3px border is intentionally thicker than the 2px used by clickable/typeable to visually distinguish the focused element.

**Step 2: No cleanup changes needed for `selected`**

The `selected` overlay uses pure CSS via the `<style>` element inside `#abp-markup-overlay`, which is already removed by `BuildMarkupCleanupScript`. No additional cleanup needed.

**Step 3: Build to verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles without errors.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat: add selected (focused element) CSS to markup injection"
```

---

### Task 3: Update `BinaryScreenshot` to Parse `disable_markup`

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:4541-4585` (BinaryScreenshot query parsing)

**Step 1: Replace `markup` query parsing with `disable_markup`**

In `BinaryScreenshot` (around line 4557-4585), find the block that parses `?markup=clickable,grid` from the query string. Replace it so that:

1. It parses `disable_markup` instead of `markup` from query params
2. It computes effective tags = all - disabled
3. If no `disable_markup` param is present, all tags are enabled by default

The current code looks like:
```cpp
// Parse markup tags from query string: ?markup=clickable,grid
std::vector<std::string> markup_tags;
// ... parsing code ...
```

Replace with:
```cpp
// Parse disable_markup from query: ?disable_markup=grid,scrollable
// Default: all markup tags enabled
std::vector<std::string> disable_tags;
std::string disable_markup_str;
// [parse disable_markup from query string the same way markup was parsed]
// ...

// Compute effective tags (all minus disabled)
std::vector<std::string> markup_tags;
if (!disable_tags.empty()) {
  // Validate disable tags
  std::string invalid_tag;
  if (!ValidateMarkupTags(disable_tags, &invalid_tag)) {
    // return error...
  }
  markup_tags = ComputeEffectiveMarkupTags(disable_tags);
} else {
  markup_tags = ComputeEffectiveMarkupTags({});  // all enabled
}
```

Make sure to update the query string parameter name from `"markup"` to `"disable_markup"` in the URL parsing logic.

**Step 2: Build to verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles without errors.

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat: BinaryScreenshot parses disable_markup, default all ON"
```

---

### Task 4: Update Action Context to Parse `disable_markup`

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h:220`
- Modify: `chrome/browser/abp/abp_action_context.cc:193-200`

**Step 1: Change default markup tags in header**

In `chrome/browser/abp/abp_action_context.h:220`, change the member variable initialization. Currently:

```cpp
std::vector<std::string> screenshot_markup_tags_;
```

Keep the declaration as-is (the default empty vector is fine), but update the constructor parsing in the .cc file to compute defaults.

**Step 2: Update parsing in action context constructor**

In `chrome/browser/abp/abp_action_context.cc:193-200`, replace the block that does `FindList("markup")` with:

```cpp
const base::Value::List* disable_list = ss_params->FindList("disable_markup");
if (disable_list) {
  std::vector<std::string> disable_tags;
  for (const auto& tag : *disable_list) {
    if (tag.is_string()) {
      disable_tags.push_back(tag.GetString());
    }
  }
  screenshot_markup_tags_ = AbpController::ComputeEffectiveMarkupTags(disable_tags);
} else {
  screenshot_markup_tags_ = AbpController::ComputeEffectiveMarkupTags({});
}
```

This ensures:
- No `screenshot` dict → all markups ON (via the default in the controller's action handler)
- `screenshot: {}` → all markups ON (no disable_markup key)
- `screenshot: {disable_markup: ["grid"]}` → all except grid

**Step 3: Verify the Screenshot() function uses action context correctly**

Check `abp_controller.cc:2056-2078` (Screenshot function). It creates an `AbpActionContext` with the params dict. The action context parses screenshot options. This should work automatically since we changed the parsing in the constructor. No changes needed to Screenshot() itself.

**Step 4: Build to verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles without errors.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "feat: action context parses disable_markup, defaults all ON"
```

---

### Task 5: Update MCP Tool Definition and Handler

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:95-106` (tool definition)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:887-917` (CallBrowserScreenshot)

**Step 1: Update tool definition**

In `abp_mcp_handler.cc:102-104`, change the tool parameter from:

```cpp
.OptionalStringArrayEnum("markup",
                         "Markup overlays to apply to the screenshot",
                         {"clickable", "typeable", "scrollable", "grid"})
```

To:

```cpp
.OptionalStringArrayEnum("disable_markup",
                         "Markup overlays to disable. All overlays are enabled by default: "
                         "clickable (green), typeable (orange), scrollable (purple dashed), "
                         "grid (red coordinate grid), selected (blue, focused element)",
                         {"clickable", "typeable", "scrollable", "grid", "selected"})
```

**Step 2: Update CallBrowserScreenshot handler**

In `abp_mcp_handler.cc:898-907`, change `args.FindList("markup")` to `args.FindList("disable_markup")`:

```cpp
base::Value::Dict screenshot_opts;
if (const base::Value::List* disable_markup = args.FindList("disable_markup")) {
  screenshot_opts.Set("disable_markup", disable_markup->Clone());
}
if (const std::string* format = args.FindString("format")) {
  screenshot_opts.Set("format", *format);
}
body_dict.Set("screenshot", std::move(screenshot_opts));
```

**Step 3: Build to verify**

Run: `autoninja -C out/Default chrome`
Expected: Compiles without errors.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat: MCP browser_screenshot uses disable_markup, default all ON"
```

---

### Task 6: Update TypeScript SDK

**Files:**
- Modify: `tools/abp-npm/src/types.ts:9-14`
- Modify: `tools/abp-npm/src/client.ts:194-198`

**Step 1: Update ScreenshotOptions type**

In `tools/abp-npm/src/types.ts:9-14`, change:

```typescript
export interface ScreenshotOptions {
  area?: "none" | "viewport";
  disable_markup?: ("clickable" | "typeable" | "scrollable" | "grid" | "selected")[];
  cursor?: boolean;
  format?: string;
}
```

**Step 2: Update screenshotBinary client method**

In `tools/abp-npm/src/client.ts:194-198`, change from `markup` to `disable_markup`:

```typescript
async screenshotBinary(tabId: string, options?: { disable_markup?: string[] }): Promise<Buffer> {
  const query = options?.disable_markup?.length
    ? `?disable_markup=${options.disable_markup.join(",")}`
    : "";
  const res = await request<Buffer>(
    `${this.baseUrl}/tabs/${tabId}/screenshot${query}`,
  );
  return res.data;
}
```

**Step 3: Build TypeScript**

Run: `cd tools/abp-npm && npm run build`
Expected: Build succeeds with no errors.

**Step 4: Commit**

```bash
git add tools/abp-npm/src/types.ts tools/abp-npm/src/client.ts
git commit -m "feat: SDK uses disable_markup with selected tag"
```

---

### Task 7: Update Debug UI

**Files:**
- Modify: `tools/abp-npm/src/debug-ui.html:656-662` (checkboxes)
- Modify: `tools/abp-npm/src/debug-ui.html:1169-1170` (JS collection)
- Modify: `tools/abp-npm/src/debug-ui.html:1138-1140` (alternative markup collection)

**Step 1: Update checkboxes to be opt-out with `selected` added**

In `tools/abp-npm/src/debug-ui.html:656-662`, replace the checkbox group. Checkboxes should be checked by default (all ON), and unchecking adds to `disable_markup`. Change:

```html
<div class="checkbox-group">
  <label><input type="checkbox" name="ss-disable-markup" value="clickable">disable clickable</label>
  <label><input type="checkbox" name="ss-disable-markup" value="typeable">disable typeable</label>
  <label><input type="checkbox" name="ss-disable-markup" value="scrollable">disable scrollable</label>
  <label><input type="checkbox" name="ss-disable-markup" value="grid">disable grid</label>
  <label><input type="checkbox" name="ss-disable-markup" value="selected">disable selected</label>
</div>
```

Note: None are checked by default. Checking a box means "disable this overlay".

**Step 2: Update JavaScript collection**

In the `buildBody()` function around line 1169-1170, change the markup collection logic from:

```javascript
const ssMarkup = $$('input[name="ss-markup"]:checked', screenshotOpts).map(function(cb) { return cb.value; });
if (ssMarkup.length > 0) ssObj.markup = ssMarkup;
```

To:

```javascript
const ssDisable = $$('input[name="ss-disable-markup"]:checked', screenshotOpts).map(function(cb) { return cb.value; });
if (ssDisable.length > 0) ssObj.disable_markup = ssDisable;
```

**Step 3: Update alternative markup field handling**

Around line 1138-1140, update the field type handler for markup checkboxes in the dynamic action fields. Change from `"markup"` field type to `"disable_markup"`:

```javascript
} else if (field.type === "disable_markup") {
  const checked = $$('input[name="disable_markup"]:checked', dynamicFields).map(function(cb) { return cb.value; });
  if (checked.length > 0) body[field.name] = checked;
```

**Step 4: Rebuild**

Run: `cd tools/abp-npm && npm run build`
Expected: Build succeeds.

**Step 5: Commit**

```bash
git add tools/abp-npm/src/debug-ui.html
git commit -m "feat: debug UI uses disable_markup checkboxes"
```

---

### Task 8: Update Claude Skill Documentation

**Files:**
- Modify: `tools/abp-claude-skill/abp-browser.md:54-56` (markup section)

**Step 1: Replace the markup overlays section**

In `tools/abp-claude-skill/abp-browser.md`, find the section about markup overlays (around lines 54-56) and replace it with:

```markdown
### Screenshot Markup Overlays

All screenshots include visual markup overlays by default. These highlight interactive elements so you can identify click/type targets:

| Overlay | Color | What it shows |
|---------|-------|---------------|
| **clickable** | Green outline | Buttons, links, clickable elements |
| **typeable** | Orange outline | Text inputs, textareas, contenteditable |
| **scrollable** | Purple dashed outline | Scrollable containers |
| **grid** | Red 100px grid | Coordinate grid with pixel labels |
| **selected** | Blue thick outline (3px) | The currently focused element |

To disable specific overlays when they're noisy, pass `disable_markup`:

```
browser_screenshot(disable_markup: ["grid", "scrollable"])
```

With no `disable_markup`, all 5 overlays are shown.
```

**Step 2: Update tool reference table**

In the tool reference table (around line 95), update the `browser_screenshot` entry to show `disable_markup` instead of `markup`:

```markdown
| `browser_screenshot` | Screenshot + wait/observe | `disable_markup`, `format` |
```

**Step 3: Commit**

```bash
git add tools/abp-claude-skill/abp-browser.md
git commit -m "docs: update Claude skill with disable_markup and color legend"
```

---

### Task 9: Update API and MCP Documentation

**Files:**
- Modify: `plans/API.md:80-95` (markup tag docs)
- Modify: `plans/API.md:438-457` (ScreenshotOptions schema)
- Modify: `plans/API.md:1138-1167` (screenshot endpoint docs)
- Modify: `plans/mcp.md:399-400` (tool reference)

**Step 1: Update markup tag documentation in API.md**

Around lines 80-95, update the markup tags table to include `selected` and note all are default-on:

```markdown
#### Markup Tags (Default: All Enabled)

All markup overlays are enabled by default. Use `disable_markup` to turn off specific ones.

| Tag | Description |
|-----|-------------|
| `clickable` | Buttons, links, and other clickable elements (green outline, 2px) |
| `typeable` | Text inputs, textareas, contenteditable elements (orange outline, 2px) |
| `scrollable` | Elements with scrollable overflow content (purple dashed outline, 2px) |
| `grid` | 100px coordinate grid overlay with pixel coordinate labels (red) |
| `selected` | The currently focused element (blue outline, 3px) |
```

**Step 2: Update ScreenshotOptions schema**

Around lines 438-457, change `markup` to `disable_markup`:

```json
"screenshot": {
  "area": "viewport",
  "disable_markup": ["grid"],
  "cursor": true
}
```

**Step 3: Update GET/POST screenshot endpoint docs**

Around lines 1138-1167, change query parameter from `markup` to `disable_markup`:

```markdown
GET /tabs/{tab_id}/screenshot

Query params:
- `disable_markup=grid,scrollable` - Comma-separated tags to disable (all enabled by default)
```

**Step 4: Update MCP doc**

In `plans/mcp.md:399-400`, no structural change needed (just tool name reference). Verify the tool listing is up to date.

**Step 5: Commit**

```bash
git add plans/API.md plans/mcp.md
git commit -m "docs: update API and MCP specs for disable_markup"
```

---

### Task 10: Integration Test

**Step 1: Launch ABP and test default markups**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/test-markup --no-first-run
```

**Step 2: Verify default screenshots have all markups**

```bash
# Take screenshot with no params — should have all 5 markups
curl http://localhost:8222/api/v1/tabs/$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json;print(json.load(sys.stdin)[0]['id'])")/screenshot -o /tmp/default.webp

# Take screenshot with grid disabled
curl "http://localhost:8222/api/v1/tabs/TAB_ID/screenshot?disable_markup=grid" -o /tmp/no-grid.webp
```

Expected: `/tmp/default.webp` shows all markup overlays. `/tmp/no-grid.webp` shows all except grid.

**Step 3: Verify MCP tool**

```bash
# Initialize MCP
curl -X POST http://localhost:8222/mcp -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

# List tools — verify browser_screenshot has disable_markup param
curl -X POST http://localhost:8222/mcp -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
```

Expected: `browser_screenshot` tool shows `disable_markup` parameter with enum values including `selected`.

**Step 4: Commit final**

```bash
git add -A
git commit -m "feat: default markup overlays with disable_markup parameter

All 5 markup overlays (clickable, typeable, scrollable, grid, selected)
are now enabled by default on all screenshots. The old markup (opt-in)
parameter is replaced with disable_markup (opt-out).

New selected overlay highlights document.activeElement with a 3px blue
border, thicker than the 2px used by other overlays."
```
