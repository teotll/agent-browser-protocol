# Drag and Drop Action

## Summary

Add a single atomic drag-and-drop action to ABP. One API call takes start and end coordinates and produces the full mouse event sequence: move to start, press, interpolated moves along path, release at end.

## API

### REST

`POST /api/v1/tabs/{id}/drag`

### MCP Tool

`browser_drag`

### Parameters

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| `start_x` | number | yes | X coordinate of drag start |
| `start_y` | number | yes | Y coordinate of drag start |
| `end_x` | number | yes | X coordinate of drop target |
| `end_y` | number | yes | Y coordinate of drop target |
| `steps` | integer | no | Intermediate mouseMoved events (default: 10, min: 1) |
| `tab_id` | string | no | Target tab (MCP only, auto-resolves) |

### Response

```json
{
  "status": "dragged",
  "start_x": 100, "start_y": 200,
  "end_x": 400, "end_y": 300
}
```

## Event Sequence

With ~5ms delays between steps:

1. `mouseMoved` to (start_x, start_y)
2. `mousePressed` at (start_x, start_y) with button="left"
3. For i = 1..steps: `mouseMoved` to linearly interpolated (x_i, y_i)
4. `mouseReleased` at (end_x, end_y)

Interpolation: `x_i = start_x + (end_x - start_x) * i / steps`

## Implementation

Uses CDP `Input.dispatchMouseEvent` — same as Click. Recursive step dispatch pattern like `TypeNextCharacter`.

### Files Changed

1. `abp_input_dispatcher.h` — Add `Drag()` and `DragNextStep()`
2. `abp_input_dispatcher.cc` — Implement drag sequence
3. `abp_controller.h` — Add `Drag()` declaration
4. `abp_controller.cc` — Route `"drag"` action, forward to input dispatcher
5. `abp_mcp_handler.h` — Add `CallBrowserDrag()` declaration
6. `abp_mcp_handler.cc` — MCP tool definition, routing, implementation

### Virtual Cursor

Updates virtual cursor to end position after drag completes (same as Click/Move).
