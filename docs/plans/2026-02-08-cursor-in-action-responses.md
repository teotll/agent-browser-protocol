# Include Virtual Cursor Position in Action Responses

**Date:** 2026-02-08

## Summary

Add a top-level `cursor` key to every action response envelope, containing the current virtual cursor position and type. Applies to both REST API and MCP responses.

## Response Format

New `cursor` field in the action response envelope:

```json
{
  "result": { "status": "clicked" },
  "cursor": {
    "x": 150.0,
    "y": 300.0,
    "cursor_type": "pointer"
  },
  "screenshot_before": { ... },
  "screenshot_after": { ... },
  "scroll": { "x": 0, "y": 120 },
  "events": [ ... ],
  "timing": { ... },
  "virtual_time": { ... }
}
```

Fields:
- `x` (double) — horizontal position in CSS pixels
- `y` (double) — vertical position in CSS pixels
- `cursor_type` (string) — one of: pointer, hand, text, etc. (mapped from `ui::mojom::CursorType`)

## Scope

- **Every action response** — click, type, navigate, scroll, execute, wait, etc.
- **Action responses only** — not tab info or other non-action endpoints
- The `active` field from `VirtualCursorState` is intentionally excluded; the cursor should always be visible.

## Implementation

**Single change point:** `AbpActionContext::SendResponse()` in `abp_action_context.cc`

This is the unified funnel where the response envelope is assembled. Read cursor state from `controller_->GetTabState(tab_id_)->cursor` and add the `cursor` dict before sending.

Add a `CursorTypeToString()` helper to map `ui::mojom::CursorType` enum values to string representations.

**MCP: no changes needed.** `OnControllerResponse` already forwards the full JSON as a text content block, so cursor data flows through automatically.
