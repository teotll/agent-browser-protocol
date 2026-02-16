# Default Markup Overlays with `disable_markup`

## Summary

Change screenshot markup overlays from opt-in (`markup: [...]`) to opt-out (`disable_markup: [...]`). All 5 markup types are ON by default. Add a new `selected` markup type that highlights the focused element with a blue 3px border.

## Motivation

Agents using ABP rarely want raw screenshots without element annotations. Making markups default-on means every screenshot is immediately actionable without the agent needing to remember to request overlays.

## Markup Tags

5 markup types, all enabled by default:

| Tag | Color | Style | What it highlights |
|-----|-------|-------|--------------------|
| `clickable` | Green #4CAF50 | 2px solid outline | `<a>`, `<button>`, `[role='button']`, `[role='link']`, `[onclick]`, `[tabindex]:not([tabindex='-1'])` |
| `typeable` | Orange #FF9800 | 2px solid outline | `<input>` (text types), `<textarea>`, `[contenteditable='true']` |
| `scrollable` | Purple #9C27B0 | 2px dashed outline | Elements with overflow scroll/auto and scrollable content |
| `grid` | Red rgba(255,0,0,0.3) | 100px grid overlay with labels | Fixed viewport overlay with coordinate labels |
| `selected` | Blue #2196F3 | **3px solid outline** | `document.activeElement` (the focused element) |

The `selected` overlay is thicker (3px) than the others (2px) to visually distinguish it.

## API Changes

### REST API

**GET /tabs/{id}/screenshot**

Before: `?markup=clickable,grid`
After: `?disable_markup=grid,scrollable` (no param = all 5 ON)

**POST /tabs/{id}/screenshot and all action envelopes**

Before:
```json
{"screenshot": {"markup": ["clickable", "typeable"], "format": "webp"}}
```

After:
```json
{"screenshot": {"disable_markup": ["grid", "scrollable"], "format": "webp"}}
```

No `screenshot` key or `disable_markup: []` = all 5 markups ON.

### MCP Tools

`browser_screenshot` parameter changes from `markup` (opt-in array) to `disable_markup` (opt-out array). Enum values: `clickable`, `typeable`, `scrollable`, `grid`, `selected`.

All action tools (click, type, navigate, etc.) return screenshots with all markups ON by default.

### TypeScript SDK

```typescript
interface ScreenshotOptions {
  disable_markup?: ("clickable" | "typeable" | "scrollable" | "grid" | "selected")[];
  // ...
}
```

## Implementation

### C++ Changes

**Constants:**
```cpp
const std::vector<std::string> kAllMarkupTags =
    {"clickable", "typeable", "scrollable", "grid", "selected"};
```

**Effective tag computation:**
```cpp
std::vector<std::string> ComputeEffectiveMarkupTags(
    const std::vector<std::string>& disable_tags) {
  std::vector<std::string> effective;
  for (const auto& tag : kAllMarkupTags) {
    if (std::find(disable_tags.begin(), disable_tags.end(), tag) == disable_tags.end())
      effective.push_back(tag);
  }
  return effective;
}
```

**Files:**
- `abp_controller.h` — Add `selected` to valid tags, add `kAllMarkupTags` constant, update `ScreenshotOptions`
- `abp_controller.cc`:
  - `IsValidMarkupTag()` — add `"selected"`
  - `BuildMarkupInjectionScript()` — add `selected` CSS: `*:focus { outline: 3px solid #2196F3 !important; outline-offset: -3px !important; }`
  - `BinaryScreenshot()` — parse `disable_markup` from query string, compute effective tags
  - `Screenshot()` — parse `disable_markup` from screenshot dict
- `abp_action_context.cc` — parse `disable_markup`, compute effective tags (default = all ON)
- `abp_action_context.h` — default `screenshot_markup_tags_` to all tags
- `abp_mcp_handler.cc` — change tool definition to `disable_markup`, update handler

### Node.js Changes

- `types.ts` — `ScreenshotOptions.markup` becomes `disable_markup` with `selected` added
- `client.ts` — `screenshotBinary()` uses `disable_markup` query param
- `debug-ui.html` — checkboxes default checked, unchecking adds to `disable_markup`

### Documentation

- `abp-browser.md` (Claude skill) — add color legend, explain default-on behavior
- `API.md` — update parameter names, examples, defaults
- `mcp.md` — update tool parameter documentation

## Approach

Clean break (no backwards compatibility). Package is v0.1.0 pre-release with no external users.
