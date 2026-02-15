# Composable Markup Tags

## Overview

Rework the screenshot markup system from a single enum (`none`, `interactive`, `clickable`, `typeable`, `inputs`) to a composable array of tags. Users pass a set of markup tags and all requested overlays are rendered together.

## Tags

| Tag | Detection | Rendering | Color |
|-----|-----------|-----------|-------|
| `clickable` | CSS selectors | `outline: 2px solid` | Green `#4CAF50` |
| `typeable` | CSS selectors | `outline: 2px solid` | Orange `#FF9800` |
| `scrollable` | JS DOM walk | `outline: 2px dashed` (via CSS class) | Purple `#9C27B0` |
| `grid` | None (static) | Fixed overlay div with gradients + labels | Semi-transparent red `rgba(255,0,0,0.3)` |

### clickable (CSS)

Selectors: `a`, `[role='link']`, `button`, `[role='button']`, `[onclick]`, `[tabindex]:not([tabindex='-1'])`

### typeable (CSS)

Selectors: `input:not([type='hidden']):not([type='checkbox']):not([type='radio']):not([type='submit']):not([type='button'])`, `textarea`, `[contenteditable='true']`

### scrollable (JS + CSS class)

Detection: JS walks visible elements, checks `scrollHeight > clientHeight || scrollWidth > clientWidth`, excluding `<body>`. Adds class `abp-scrollable` to matching elements. CSS rule targets `.abp-scrollable`.

Dashed outline distinguishes scrollable containers from solid element outlines.

Highlights nested overflow containers only (inner scrollable divs, iframes). Page-level scroll is implicit and not marked.

### grid (DOM overlay)

Injects an absolutely-positioned, pointer-events-none `<div>` covering the viewport. Grid lines at 100px intervals using `repeating-linear-gradient` on both axes. Coordinate labels ("100", "200", etc.) along top and left edges as small positioned elements.

Helps agents map visual position to x,y pixel coordinates for mouse actions.

## API

### Breaking change

The `markup` parameter changes from a string to an array of strings. Old values (`none`, `interactive`, `inputs`) are removed.

### POST body

```json
{
  "screenshot": {
    "markup": ["clickable", "typeable", "grid"]
  }
}
```

### GET query parameter

Comma-separated:

```
GET /api/v1/tabs/{id}/screenshot?markup=clickable,grid
```

### MCP tool

Uses `OptionalStringArrayEnum` (already supported by `ToolBuilder`):

```cpp
.OptionalStringArrayEnum("markup",
    "Markup overlays to apply",
    {"clickable", "typeable", "scrollable", "grid"})
```

### Validation

Unknown tags return HTTP 400. Empty array or omitted field means no markup.

## Implementation

### Rendering approach

All tags use DOM injection via `Runtime.evaluate` with `disableBreaks: true`. No CDP Overlay domain. Temporary injection cleaned up after screenshot capture.

### Injection (single Runtime.evaluate)

1. Create wrapper: `<div id="abp-markup-overlay">`
2. Build `<style>` with CSS rules for active pure-CSS tags (`clickable`, `typeable`)
3. If `scrollable`: JS walks DOM, adds `abp-scrollable` class to matching elements, includes `.abp-scrollable` rule in style block
4. If `grid`: creates overlay div with `repeating-linear-gradient` and coordinate label elements, appends to wrapper
5. Append wrapper to `document.documentElement`

Wrapper uses `position: fixed; inset: 0; z-index: 2147483647; pointer-events: none`.

### Cleanup (single Runtime.evaluate, fire-and-forget)

1. Remove `<div id="abp-markup-overlay">`
2. If `scrollable` was active: remove `abp-scrollable` class from all elements

### Screenshot flow (unchanged)

```
Inject markup -> ForceRedraw -> 167ms wait -> GrabViewSnapshot -> Cleanup
```

## Files to change

### C++ (chrome/browser/abp/)

- `abp_controller.h` — `ScreenshotOptions::markup` from `std::string` to `std::vector<std::string> markup_tags`
- `abp_controller.cc` — Replace `if/else if` markup chain with JS builder that composes from tag list. Update request parsing (POST array, GET comma-separated). Update cleanup to handle scrollable class removal.
- `abp_action_context.h` — `screenshot_markup_` from `std::string` to `std::vector<std::string>`
- `abp_action_context.cc` — Parse `markup` as array from action envelope
- `abp_mcp_handler.cc` — Change `browser_screenshot` tool definition to `OptionalStringArrayEnum`. Update argument extraction to read array.
- `abp_types.h` — Update if markup type defined here

### npm package (tools/abp-npm/)

- `src/types.ts` — `ScreenshotOptions.markup` from `string` to `string[]`
- `src/client.ts` — `screenshotBinary` joins array as comma-separated for GET query param

### Documentation

- `plans/API.md` — Update screenshot parameter docs
- `tools/abp-npm/README.md` — Update examples
- `tools/abp-claude-skill/abp-browser.md` — Update markup tool reference
