# Console Capture via MCP — Design Spec

## Problem

AI agents using ABP have no visibility into the JavaScript console. When a page doesn't behave as expected, the agent can't check for errors, warnings, or logged messages — it can only see the visual output via screenshots. Human developers routinely use the console for debugging; agents need the same capability.

## Requirements

- Expose JS console messages (all levels) via MCP tool and REST endpoint
- Capture everything a human sees in DevTools Console: `console.*` calls, uncaught exceptions, CORS errors, CSP violations, mixed content warnings
- No ABP action cycle required — agent queries on demand
- **Not fingerprintable** — no CDP domain enabling (`Runtime.enable`), no JS injection
- In-memory only — no SQLite persistence
- 5000-entry FIFO ring buffer

## Architecture

Single capture path using Chromium's existing renderer→browser console message IPC.

```
console.*() / CORS / CSP / uncaught exceptions  (renderer)
       │
       ▼
FrameConsole::ReportMessageToClient (renderer)
       │  [kNetwork source filter removed — ABP fork change]
       ▼
ChromeClientImpl::AddMessageToConsole (renderer)
       │
       ▼  Mojo IPC: DidAddMessageToConsole
       │
WebContentsImpl (browser process)
       │
       ▼
WebContentsObserver::OnDidAddMessageToConsole
       │
       ▼
AbpConsoleCapture (browser process)
  - 5000-entry ring buffer
  - Monotonic ID per entry
  - Queryable by level, tab, text pattern
       │
       ├── GET  /api/v1/console   (REST query)
       ├── DELETE /api/v1/console  (REST clear)
       └── MCP browser_console    (tool)
```

### Why not CDP `Runtime.enable`?

`Runtime.enable` is detectable by page JavaScript — it's a fingerprinting vector. ABP operates at the engine level specifically to avoid observable side effects.

### Why not JS injection?

Monkey-patching `console.*` via `Page.addScriptToEvaluateOnNewDocument` is fragile (pages can clobber it), doesn't survive cross-origin navigations cleanly, and adds observable artifacts to the page's JS context.

### Why single path over Log.enable hybrid?

`Log.enable` (CDP) is not fingerprintable and would provide granular `source` tags (network, security, deprecation, etc.). However, by removing the `kNetwork` source filter in the renderer, all console messages — including CORS and CSP — flow through the same `WebContentsObserver` path. This gives us:

- One capture path instead of two
- Zero CDP involvement
- Simpler controller code
- No CDP event routing needed

The trade-off: we lose the CDP `Log.entryAdded` source categorization (network vs security vs deprecation). The `OnDidAddMessageToConsole` Mojo IPC does not carry the `ConsoleMessageSource` enum — only level, message, line number, source URL, and stack trace. The message text itself is sufficient for agents to distinguish error types.

## Renderer-Side Change

**File:** `third_party/blink/renderer/core/frame/frame_console.cc`

In `FrameConsole::ReportMessageToClient`, remove the early-return for `kNetwork` source messages.

```cpp
// Before:
if (source == mojom::blink::ConsoleMessageSource::kNetwork)
  return;

// After:
// ABP: Forward network-source console messages (CORS, CSP, mixed content)
// to the browser process via DidAddMessageToConsole. Stock Chromium filters
// these out since they're redundant with the Log domain, but ABP captures
// console messages via WebContentsObserver to avoid enabling CDP domains
// that could be fingerprinted by page JavaScript.
```

## Data Model

### ConsoleEntry (abp_types.h)

```cpp
struct ConsoleEntry {
  int64_t id;                    // Monotonic sequence number for pagination
  std::string tab_id;            // DevToolsAgentHost ID of source tab
  std::string level;             // "verbose", "info", "warning", "error"
  std::string message;           // Message text
  int32_t line_number;           // Source line number (0 if unavailable)
  std::string source_url;        // Script URL or empty
  std::string stack_trace;       // Stack trace string (errors only, if available)
  int64_t timestamp_ms;          // Wall clock milliseconds since epoch
};
```

### JSON representation

```json
{
  "id": 57,
  "tab_id": "ABC123",
  "level": "error",
  "message": "Access to XMLHttpRequest at 'https://api.example.com' from origin 'https://example.com' has been blocked by CORS policy",
  "line_number": 0,
  "source_url": "",
  "stack_trace": "",
  "timestamp_ms": 1741700000000
}
```

## REST API

### GET /api/v1/console

Query buffered console messages.

**Query parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `level` | string | (all) | Minimum level filter: `verbose` < `info` < `warning` < `error` |
| `pattern` | string | (none) | Regex match against message text (case-insensitive) |
| `tab_id` | string | (all) | Filter to specific tab |
| `limit` | int | 100 | Max entries to return |
| `after_id` | int | 0 | Return entries with `id` > this value |

**Response:**

```json
{
  "entries": [ ... ],
  "total_buffered": 312,
  "oldest_id": 1
}
```

- `total_buffered`: current number of entries in the buffer
- `oldest_id`: lowest id still in buffer (entries below this were evicted)

The agent can use `after_id` to poll efficiently — remember the last `id` seen, pass it on next query to get only new messages.

### DELETE /api/v1/console

Clear the buffer.

**Query parameters:**

| Param | Type | Default | Description |
|-------|------|---------|-------------|
| `tab_id` | string | (all) | Clear only entries from this tab |

**Response:**

```json
{
  "cleared": 312
}
```

## MCP Tool

### browser_console

```json
{
  "name": "browser_console",
  "description": "Query JavaScript console messages including logs, errors, warnings, and browser messages (CORS, CSP). Use to debug page behavior without requiring an action cycle.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "level": {
        "type": "string",
        "enum": ["verbose", "info", "warning", "error"],
        "description": "Minimum severity level to return"
      },
      "pattern": {
        "type": "string",
        "description": "Regex filter on message text (case-insensitive)"
      },
      "tab_id": {
        "type": "string",
        "description": "Filter to specific tab ID"
      },
      "limit": {
        "type": "integer",
        "default": 100,
        "description": "Maximum entries to return"
      },
      "after_id": {
        "type": "integer",
        "description": "Return only entries with id greater than this value"
      },
      "clear": {
        "type": "boolean",
        "description": "Clear the buffer (optionally filtered by tab_id)"
      }
    }
  }
}
```

When `clear: true` is set, the tool performs a clear operation (using `tab_id` if provided) and returns `{"cleared": N}`. All other parameters (`level`, `pattern`, `limit`, `after_id`) are ignored. When `clear` is false or omitted, the tool performs a query. The two operations are mutually exclusive.

## Implementation Components

### New files

| File | Purpose |
|------|---------|
| `chrome/browser/abp/abp_console_capture.h` | `AbpConsoleCapture` class and `AbpConsoleObserver` declaration |
| `chrome/browser/abp/abp_console_capture.cc` | Ring buffer, ingestion, query, clear, per-tab observer |

### Modified files

| File | Change |
|------|--------|
| `chrome/browser/abp/abp_types.h` | Add `ConsoleEntry` struct |
| `chrome/browser/abp/abp_controller.h` | Add `AbpConsoleCapture` member, console request handler declaration |
| `chrome/browser/abp/abp_controller.cc` | Create `AbpConsoleCapture`, manage `AbpConsoleObserver` per tab in `OnTabStripModelChanged`, add `HandleConsoleRequest()` |
| `chrome/browser/abp/abp_http_server.cc` | Route `GET /api/v1/console` and `DELETE /api/v1/console` to controller |
| `chrome/browser/abp/abp_mcp_handler.cc` | Add `browser_console` tool definition and `CallBrowserConsole()` handler |
| `chrome/browser/abp/BUILD.gn` | Add new source files |
| `third_party/blink/renderer/core/frame/frame_console.cc` | Remove `kNetwork` source early-return |

### AbpConsoleCapture class

```cpp
class AbpConsoleCapture {
 public:
  static constexpr size_t kMaxBufferSize = 5000;

  AbpConsoleCapture();
  ~AbpConsoleCapture();

  // Ingest from WebContentsObserver::OnDidAddMessageToConsole
  void OnConsoleMessage(const std::string& tab_id,
                        blink::mojom::ConsoleMessageLevel level,
                        const std::u16string& message,
                        int32_t line_number,
                        const std::u16string& source_id,
                        const std::optional<std::u16string>& stack_trace);

  // Query with filters
  std::vector<ConsoleEntry> Query(const std::string& tab_id,
                                  const std::string& min_level,
                                  const std::string& pattern,
                                  int limit,
                                  int64_t after_id) const;

  // Clear buffer (optionally per-tab)
  size_t Clear(const std::string& tab_id);

  // Buffer stats
  size_t Size() const;
  int64_t OldestId() const;

 private:
  std::string LevelToString(blink::mojom::ConsoleMessageLevel level) const;
  int LevelPriority(const std::string& level) const;

  std::deque<ConsoleEntry> buffer_;
  int64_t next_id_ = 1;
};
```

### Per-tab WebContentsObserver

`AbpController` is a `TabStripModelObserver`, not a `WebContentsObserver`. To receive `OnDidAddMessageToConsole`, a new per-tab observer is needed.

**`AbpConsoleObserver`** — a lightweight `WebContentsObserver` subclass, one per tab. Created in `OnTabStripModelChanged` when a tab is inserted, destroyed when removed. Forwards console messages to the shared `AbpConsoleCapture` buffer.

```cpp
// Per-tab observer, created/destroyed by AbpController on tab insert/remove
class AbpConsoleObserver : public content::WebContentsObserver {
 public:
  AbpConsoleObserver(content::WebContents* web_contents,
                     const std::string& tab_id,
                     AbpConsoleCapture* capture);
  ~AbpConsoleObserver() override;

  // content::WebContentsObserver:
  void OnDidAddMessageToConsole(
      content::RenderFrameHost* source_frame,
      blink::mojom::ConsoleMessageLevel level,
      const std::u16string& message,
      int32_t line_no,
      const std::u16string& source_id,
      const std::optional<std::u16string>& stack_trace) override;

 private:
  std::string tab_id_;
  raw_ptr<AbpConsoleCapture> capture_;  // Owned by AbpController
};
```

The controller manages a `std::map<std::string, std::unique_ptr<AbpConsoleObserver>>` keyed by tab_id. This keeps the observer lifecycle tied to tab lifetime without adding `WebContentsObserver` inheritance to `AbpController`.

### Threading model

All operations on `AbpConsoleCapture` run on the UI thread. `OnDidAddMessageToConsole` is called on the UI thread (after Mojo IPC from renderer). `HandleConsoleRequest` is posted to the UI thread from the IO thread by `AbpHttpServer`. No synchronization needed.

## Level Priority

For the `level` filter parameter, levels follow standard severity ordering:

| Level | Priority |
|-------|----------|
| `verbose` | 0 |
| `info` | 1 |
| `warning` | 2 |
| `error` | 3 |

`level=warning` returns warning and error entries. `level=verbose` returns everything.

## Edge Cases

- **Tab closed**: Entries remain in buffer with the closed tab's ID. The agent can still query them. They'll eventually be evicted by FIFO.
- **Navigation**: Buffer persists across navigations — no reset. The `source_url` field shows which page produced each message.
- **Iframes**: Console messages from iframes route through the same `WebContentsObserver` on the parent's `WebContents`. They'll have the iframe's source URL in `source_url`.
- **Buffer full**: Oldest entries evicted first. `oldest_id` in the response tells the agent if it missed anything.
- **Empty stack trace**: Only error-level messages may have stack traces, and only if `SetWantErrorMessageStackTrace()` was called. Most entries will have empty `stack_trace`.
- **Empty buffer**: `oldest_id` returns 0 when the buffer is empty (IDs start at 1).
- **Invalid regex**: `pattern` uses RE2 (guarantees linear-time matching). Invalid patterns return HTTP 400.
- **`source_id` naming**: The Chromium IPC parameter `source_id` is actually a URL string. It maps to `source_url` in `ConsoleEntry`.

## Testing

- Browser test: navigate to a page that calls `console.log/warn/error`, query the REST endpoint, verify entries
- Browser test: navigate to a page with CORS violation, verify the error appears in the buffer
- Browser test: verify `after_id` pagination and `level` filtering
- Browser test: verify `DELETE` clears the buffer
- Browser test: fill beyond 5000 entries, verify FIFO eviction and `oldest_id` advances
- Browser test: verify `after_id` with evicted entries returns correct results
- Browser test: verify iframe console messages appear with iframe's `source_url`
- Browser test: verify per-tab `DELETE` only removes targeted tab's entries
- Browser test: verify invalid regex returns HTTP 400
- Manual test: MCP tool via Claude Desktop
