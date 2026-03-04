# Design: Multi-Scroll with Intermediate Screenshots

**Date:** 2026-03-03
**Status:** Approved

## Overview

Extend `browser_scroll` (MCP tool + REST API) to accept up to 3 scroll events in a single action, capturing a screenshot after each scroll. This lets an agent scroll multiple viewport heights and gather screenshots for all positions in one action loop.

## Schema Changes

`browser_scroll` params — top-level `x`/`y` are shared across all scrolls. Single-scroll usage (`delta_x`/`delta_y` at top level) remains backward compatible.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `x` | number | yes | X coordinate where scroll wheel fires (shared across all scrolls) |
| `y` | number | yes | Y coordinate where scroll wheel fires (shared across all scrolls) |
| `delta_x` | number | no | Single-scroll horizontal delta (backward compat) |
| `delta_y` | number | no | Single-scroll vertical delta (backward compat) |
| `scrolls` | array | no | Up to 3 `{delta_x?, delta_y?}` objects for multi-scroll |
| `tab_id` | string | no | Target tab ID |

Validation: either `scrolls` or at least one of `delta_x`/`delta_y` must be present. `scrolls` max length 3. Each `scrolls` item must have at least one non-zero delta.

## MCP Tool Description

```
Scroll using mouse wheel at element coordinates. Simulates moving mouse over
element and scrolling. At least one of delta_x or delta_y must be non-zero.
Optionally accepts a scrolls array of up to 3 {delta_x, delta_y} objects to
scroll multiple viewport heights in one action — a screenshot is captured after
each scroll and returned as sequential image blocks.
```

## Execution Flow

**Single scroll (existing behavior, unchanged):**
```
Resume → ForwardWheelEvent → OnActionDispatched
→ AbpActionContext: wait(500ms) → ForceRedraw → pause → screenshot_after
```

**Multi-scroll (new `scrolls` array path):**
```
Resume
For scrolls[0..N-2] (all but last):
  ForwardWheelEvent(x, y, delta_x, delta_y)
  PostDelayedTask(500ms)                         ← wait for scroll to render
  CaptureScreenshotFromBuffer(tab_id, ...)       ← intermediate capture
  Append {data, width, height} to intermediate_screenshots

For scrolls[N-1] (last):
  ForwardWheelEvent(x, y, delta_x, delta_y)
  ctx->OnActionDispatched()
  → AbpActionContext: wait(500ms) → ForceRedraw → pause → screenshot_after
```

The `intermediate_screenshots` array is placed inside the `result` dict. The final screenshot comes from `screenshot_after` via the normal AbpActionContext lifecycle.

## Response Format

**REST response:**
```json
{
  "action_id": "...",
  "tab_id": "...",
  "result": {
    "status": "scrolled",
    "scrolls_executed": 3,
    "intermediate_screenshots": [
      {"data": "<base64>", "width": 2070, "height": 1672, "format": "webp"},
      {"data": "<base64>", "width": 2070, "height": 1672, "format": "webp"}
    ]
  },
  "screenshot_after": {"data": "<base64>", "width": 2070, "height": 1672, "format": "webp"},
  "scroll": {...},
  "timing": {...}
}
```

**MCP content array** — `OnControllerResponse` detects `intermediate_screenshots` in `result`, strips image data, and emits one image block per entry followed by the `screenshot_after` image block:

```json
[
  {"type": "text", "text": "{...json without image data...}"},
  {"type": "image", "data": "<scroll1_base64>", "mimeType": "image/webp"},
  {"type": "image", "data": "<scroll2_base64>", "mimeType": "image/webp"},
  {"type": "image", "data": "<scroll3_base64>", "mimeType": "image/webp"}
]
```

For single-scroll (backward compat), the content array is unchanged: `[text, image]`.

## Files to Modify

1. **`chrome/browser/abp/abp_mcp_handler.cc`**
   - Update `browser_scroll` tool schema: make `delta_x`/`delta_y` optional, add `scrolls` array parameter
   - Update tool description
   - `OnControllerResponse`: extract `intermediate_screenshots` from `result.intermediate_screenshots`, strip `data` fields from JSON, emit as image blocks before `screenshot_after`

2. **`chrome/browser/abp/abp_input_dispatcher.cc`**
   - `Scroll()`: detect `scrolls` array
   - Implement multi-scroll loop: for each non-final scroll, dispatch wheel event + 500ms delay + `CaptureScreenshotFromBuffer`
   - Collect results into `intermediate_screenshots` array in result dict
   - Final scroll calls `OnActionDispatched()` as normal

## Non-Goals

- No new tool (extends existing `browser_scroll`)
- No changes to AbpActionContext lifecycle
- No per-scroll "before" screenshots
- No intermediate pause/resume cycles (intermediate captures happen while page is resumed)
