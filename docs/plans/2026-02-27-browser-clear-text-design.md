# Design: `browser_clear_text` MCP Tool Macro

## Problem

Agents struggle with clearing text in input fields using repeated backspace/delete operations. A dedicated macro reliably clears focused inputs.

## MCP Tool Schema

```
browser_clear_text(tab_id?, x, y)
```

- `x`, `y` (required): center of the input field. A click is sent first to focus the element.
- `tab_id` (optional): defaults to active tab.

## Strategy

- **Backspace-only** — no Ctrl+A. More compatible with custom inputs, contenteditable, and widgets that intercept selection shortcuts.
- **Batch loop** — 20 backspace keystrokes per batch at 240 WPM. After each batch, check if input is cleared via JS. Repeat up to 1000 total keystrokes (50 batches).
- **Native input** — uses `ForwardKeyEvent` (same as Type/KeyPress actions), not CDP key events.

## Architecture

```
MCP browser_clear_text → CallBrowserClearText
  → REST POST /api/v1/tabs/{id}/clear_text
  → AbpController::ClearText
  → AbpInputDispatcher::ClearText
```

Wrapped in `AbpActionContext::RunWithOptions` for full action lifecycle (resume → before screenshot → action → wait → after screenshot → pause → response).

## Action Logic

1. **Click to focus** — `mousePressed` + `mouseReleased` at `(x, y)` via CDP `Input.dispatchMouseEvent`. Update virtual cursor position.

2. **ClearTextBatch(ctx, total_sent)** — entry point for each batch of 20 keystrokes.
   - Exit if `total_sent >= 1000`.
   - Send Backspace keyDown via `ForwardKeyEvent`.
   - Schedule keyUp after 20ms dwell.

3. **ClearTextKeyUp(ctx, total_sent, batch_count)** — after each key dwell.
   - Send Backspace keyUp.
   - Schedule next key after 30ms gap (50ms total per keystroke = 240 WPM).

4. **ClearTextNextKey(ctx, total_sent, batch_count)** — between keystrokes.
   - If `batch_count < 20` → send next keyDown, loop to step 3.
   - If `batch_count >= 20` → check if cleared.

5. **Check** — CDP `Runtime.evaluate` with `disableBreaks: true`:
   ```js
   (function(){var e=document.activeElement;if(!e)return '';
   if(typeof e.value==='string')return e.value;return e.textContent||'';})()
   ```
   - Empty → done.
   - Not empty → call ClearTextBatch for next batch.

## Timing

- Per keystroke: 20ms dwell + 30ms gap = 50ms
- Per batch (20 keys): ~1 second
- Worst case (1000 keys): ~50 seconds
- Typical (< 100 chars): ~5 seconds

## Response

```json
{"status": "cleared", "keystrokes_sent": 40}
```

## Files to Modify

- `chrome/browser/abp/abp_mcp_handler.h` — `CallBrowserClearText` declaration
- `chrome/browser/abp/abp_mcp_handler.cc` — tool schema, dispatch routing, Call implementation, system prompt
- `chrome/browser/abp/abp_controller.h` — `ClearText` declaration
- `chrome/browser/abp/abp_controller.cc` — REST routing, thin forwarder
- `chrome/browser/abp/abp_input_dispatcher.h` — `ClearText` + helper declarations
- `chrome/browser/abp/abp_input_dispatcher.cc` — core implementation
