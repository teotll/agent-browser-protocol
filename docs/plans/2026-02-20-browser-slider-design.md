# Browser Slider Macro

## Problem

Browsing agents struggle with sliders because they require coordinated mousedown, interpolated mousemoves, and mouseup — plus pixel-level math to map logical values (e.g. "$50") to screen coordinates. This multi-step operation frequently fails or requires multiple retries.

## Solution

A single `slider` endpoint that accepts the slider's track geometry, current thumb position, value range, and target value. The endpoint calculates the target pixel position via linear interpolation and delegates to the existing `DragRaw()` infrastructure.

## API

**REST Endpoint:** `POST /api/v1/tabs/{id}/slider`

**MCP Tool:** `slider`

Discriminated union on `orientation`:

### Horizontal

```json
{
  "orientation": "horizontal",
  "y": 350,
  "x_start": 100,
  "x_end": 500,
  "current_x": 200,
  "min": 0,
  "max": 100,
  "target_value": 75
}
```

| Field | Type | Description |
|-------|------|-------------|
| `orientation` | `"horizontal"` | Discriminator |
| `y` | number | Y coordinate of the slider track |
| `x_start` | number | Left edge of the track (pixel) |
| `x_end` | number | Right edge of the track (pixel) |
| `current_x` | number | Current thumb X position (pixel) |
| `min` | number | Minimum logical value |
| `max` | number | Maximum logical value |
| `target_value` | number | Desired logical value |

### Vertical

```json
{
  "orientation": "vertical",
  "x": 350,
  "y_start": 100,
  "y_end": 500,
  "current_y": 200,
  "min": 0,
  "max": 100,
  "target_value": 75
}
```

| Field | Type | Description |
|-------|------|-------------|
| `orientation` | `"vertical"` | Discriminator |
| `x` | number | X coordinate of the slider track |
| `y_start` | number | Top edge of the track (pixel) |
| `y_end` | number | Bottom edge of the track (pixel) |
| `current_y` | number | Current thumb Y position (pixel) |
| `min` | number | Minimum logical value |
| `max` | number | Maximum logical value |
| `target_value` | number | Desired logical value |

## Interpolation

```
ratio = (target_value - min) / (max - min)
target_position = track_start + ratio * (track_end - track_start)
clamp(target_position, track_start, track_end)
```

## Implementation

Thin wrapper in `AbpInputDispatcher`:

1. Parse and validate discriminated params
2. Calculate target pixel position via linear interpolation
3. Clamp to track bounds
4. Build drag params from current position to target position
5. Call `DragRaw()` with 10 interpolation steps

### Files Modified

- `abp_input_dispatcher.h/cc` — `Slider()` and `SliderRaw()` methods
- `abp_controller.cc` — route `/slider` to dispatcher
- `abp_http_server.cc` — register POST route
- `abp_mcp_handler.cc` — register `slider` MCP tool
- `abp_tool_builder.cc` — tool schema

### Error Handling

All validation errors return 400:

| Condition | Message |
|-----------|---------|
| `min >= max` | "min must be less than max" |
| `target_value` out of `[min, max]` | "target_value must be between min and max" |
| `track_start == track_end` | "track start and end must be different" |
| `current_position` outside track | "current position must be within track bounds" |
| Missing required field | "horizontal orientation requires y, x_start, x_end, current_x" |
| Unexpected field for orientation | "unexpected field y_start for horizontal orientation" |
| Invalid orientation | "orientation must be horizontal or vertical" |

## Testing

Browser test with `<input type="range">` test page:

- Horizontal slider drag executes successfully
- Vertical slider drag executes successfully
- Validation errors return 400 for all invalid param combinations

Test file: `abp_action_lifecycle_browsertest.cc`
Test page: `chrome/browser/abp/test_pages/slider_test.html`
