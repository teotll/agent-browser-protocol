# Network Capture & Browser Curl — Design

## Goal

Let agents understand and directly call a website's backend APIs by capturing network traffic during action cycles and providing a session-aware HTTP client (`browser_curl`) that works while JS is paused.

## Architecture

### New Components

| Component | File | Responsibility |
|-----------|------|---------------|
| `AbpNetworkCapture` | `abp_network_capture.h/cc` | Per-tab in-memory buffer. Subscribes to CDP `Network.*` events, captures full request/response metadata. Cleared on next action start (except `browser_wait` which merges). |
| `AbpNetworkDatabase` | `abp_network_database.h/cc` | SQLite storage (`network.db` in session dir) for tagged network calls. Query with regex filtering. |
| `AbpCurlHandler` | `abp_curl_handler.h/cc` | Executes HTTP requests via `network::SimpleURLLoader` on the tab's `StoragePartition`. Works while JS is paused. |

### Data Flow

```
CDP Network events (per tab)
  → AbpNetworkCapture (in-memory buffer, enriched with headers/body/timing)
  → Response body fetched eagerly via Network.getResponseBody on loadingFinished
  → Action response envelope: network counts only (total/completed/pending)
  → Agent tags (pre-declare or retroactive) → AbpNetworkDatabase (SQLite)
  → Agent queries → GET /api/v1/network (reads from DB, regex filters)
  → Agent curls → AbpCurlHandler → response + opt-in save to DB
```

## In-Memory Buffer Lifecycle

```
Action 1 (click):
  ├─ Clear buffer (fresh start)
  ├─ Capture 5 XHR calls during action
  ├─ Response: network.total=5, completed=4, pending=1
  └─ Buffer holds 5 requests

Action 2 (browser_wait):
  ├─ DO NOT clear buffer (wait merges)
  ├─ 3 more calls during wait, pending one completes
  ├─ Response: network.total=8, completed=8, pending=0
  └─ Buffer holds 8 requests

Action 3 (click):
  ├─ Clear buffer (new non-wait action)
  ├─ Capture 2 XHR calls
  ├─ Response: network.total=2, completed=2, pending=0
  └─ Buffer holds 2 requests
```

**Rules:**
- `browser_wait` — preserves and merges into existing buffer
- All other actions — clear buffer, start fresh
- Buffer lives on `TabState` (per-tab)
- Tagging (pre-declare or retroactive) flushes buffer to SQLite
- Retroactive save must happen before the next non-wait action clears the buffer

## CDP Events Captured

| Event | Data Captured |
|-------|--------------|
| `Network.requestWillBeSent` | URL, method, headers, postData (request body), resource type, timestamp. If `redirectResponse` present, update existing entry with redirect info and track final URL. |
| `Network.responseReceived` | Status code, response headers, resource type |
| `Network.loadingFinished` | Mark completed, eagerly fetch body via `Network.getResponseBody` |
| `Network.loadingFailed` | Mark completed with error, no body |
| `Network.requestWillBeSentExtraInfo` | Browser-added headers (cookies not visible in `requestWillBeSent`) |
| `Network.requestServedFromServiceWorker` | Mark request as SW-served; skip capture (not a real network call) |

**Filtering at capture time:**
- Check `resourceType` against configured types (default: `XHR`, `Fetch`)
- If not in configured list, skip — don't buffer
- **CORS preflights**: Filter out `OPTIONS` requests that are CORS preflights. Mark the associated request with `cors_preflight: true` boolean.
- **Service workers**: Skip requests served from service worker cache. Only capture the final outbound request that actually hits the network.

**Redirect handling:**
- Redirects are grouped into a single `CapturedRequest` entry
- `Network.requestWillBeSent` with `redirectResponse` updates the existing entry (same request ID)
- Final response (status, headers, body) dominates the entry
- `redirect_chain` field stores intermediate URLs/status codes for reference

**Eager body fetch:**
- On `Network.loadingFinished`, immediately call `Network.getResponseBody`
- Stores body in the in-memory `CapturedRequest` before any navigation can destroy renderer cache
- `Network.enable` called with `maxPostDataSize: 52428800` (50MB) to capture request bodies
- Skip body fetch for non-text MIME types over 50MB (safety valve)

**Buffer limits:**
- Maximum 1000 requests per tab in-memory buffer
- When limit reached, oldest requests are evicted (FIFO)
- Evicted requests are lost unless previously tagged/saved

## Data Model — SQLite Schema

**File:** `{session_dir}/network.db`

```sql
CREATE TABLE network_requests (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    request_id TEXT NOT NULL,        -- CDP request ID (reference only)
    action_id TEXT NOT NULL,
    tab_id TEXT NOT NULL,
    tag TEXT NOT NULL,

    -- Request
    url TEXT NOT NULL,               -- final URL (after redirects)
    url_hostname TEXT NOT NULL,
    url_path TEXT NOT NULL,
    url_query TEXT,
    method TEXT NOT NULL,
    request_headers TEXT,            -- JSON object
    request_body TEXT,
    resource_type TEXT,              -- xhr, fetch, document, etc.
    cors_preflight INTEGER DEFAULT 0, -- 1 if CORS preflight was sent

    -- Response
    status INTEGER,
    response_headers TEXT,           -- JSON object
    response_body TEXT,              -- text or base64-encoded binary
    response_body_encoding TEXT,     -- "text" or "base64"

    -- Redirects
    redirect_chain TEXT,             -- JSON array of {url, status} objects

    -- Metadata
    started_at_ms INTEGER,
    completed_at_ms INTEGER,
    duration_ms INTEGER,
    virtual_time_ms INTEGER,

    UNIQUE(tab_id, request_id, tag)
);

CREATE INDEX idx_tag ON network_requests(tag);
CREATE INDEX idx_action_id ON network_requests(action_id);
CREATE INDEX idx_tab_id ON network_requests(tab_id);
CREATE INDEX idx_url ON network_requests(url);
CREATE INDEX idx_hostname ON network_requests(url_hostname);
CREATE INDEX idx_path ON network_requests(url_path);
```

**In-memory struct:**

```cpp
struct CapturedRequest {
  std::string request_id;
  std::string action_id;
  std::string url;               // final URL (after redirects)
  std::string url_hostname;
  std::string url_path;
  std::string url_query;
  std::string method;
  base::Value::Dict request_headers;
  std::string request_body;
  std::string resource_type;
  bool cors_preflight = false;

  int status_code = 0;
  base::Value::Dict response_headers;
  std::string response_body;
  bool response_body_is_base64 = false;  // true for binary responses

  // Redirect chain: [{url, status}, ...]
  base::Value::List redirect_chain;

  int64_t started_at_ms = 0;
  int64_t completed_at_ms = 0;
  int64_t virtual_time_ms = 0;

  bool completed = false;
  bool served_from_service_worker = false;  // skip if true
};
```

## API Surface

### Modified Action Envelope

All action endpoints gain an optional `network` parameter:

**Request:**
```json
{
  "network": {
    "tag": "login-flow",
    "types": ["xhr", "fetch"]
  }
}
```

**Response — new `network` field:**
```json
{
  "network": {
    "total": 12,
    "completed": 10,
    "pending": 2,
    "tag": "login-flow"
  }
}
```

### New REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v1/network` | Query saved network calls with regex filters |
| POST | `/api/v1/network/save` | Retroactively tag & persist in-memory buffer |
| DELETE | `/api/v1/network` | Clear saved calls (by tag or all) |
| POST | `/api/v1/tabs/{id}/curl` | Execute HTTP request using tab's session |

### New MCP Tools

**`browser_network`** — unified query/save/clear:

| Param | Required | Description |
|-------|----------|-------------|
| `action` | yes | `"query"`, `"save"`, `"clear"` |
| `tag` | for save/clear | Tag name. For query, filters by tag |
| `url` | query | Regex on full URL |
| `hostname` | query | Regex on hostname |
| `path` | query | Regex on path |
| `query` | query | Regex on query string |
| `method` | query | Regex on HTTP method |
| `status` | query | Regex on status code |
| `type` | query | Resource type filter |
| `tab_id` | query/save | Tab scope |
| `action_id` | query | Filter by action |
| `include_body` | query | Include bodies (default: false) |

**`browser_curl`** — session-aware HTTP client:

| Param | Required | Description |
|-------|----------|-------------|
| `tab_id` | yes | Tab whose session/cookies to use |
| `url` | yes | Request URL |
| `method` | no | HTTP method (default: GET) |
| `headers` | no | Additional/override headers |
| `body` | no | Request body |
| `tag` | no | Persist to DB with this tag |

**Response:**
```json
{
  "status": 200,
  "headers": {"content-type": "application/json"},
  "body": "{\"user\": ...}",
  "body_encoding": "text",
  "url": "https://api.example.com/users",
  "redirected": false
}
```

For binary responses, `body` is base64-encoded and `body_encoding` is `"base64"`. In MCP, binary bodies are returned as image/blob content blocks instead of text.

### browser_curl Implementation

- `network::SimpleURLLoader` on browser IO thread
- Uses tab's `StoragePartition::GetURLLoaderFactoryForBrowserProcess()` — inherits cookies, session, auth
- Works while JS/virtual time is paused (browser process, independent of renderer)
- Not an action — no action lifecycle, no pause/resume, no screenshot
- Follows redirects by default
- **Auto-populates `Origin` and `Referer`** from tab's current URL so the request looks like it was triggered by page JS
- **Binary responses**: base64-encoded in REST response (`response_body_encoding: "base64"`). In MCP, rendered as image/blob content blocks so the agent can "see" the response.

## File Layout

**New files:**

| File | Responsibility |
|------|---------------|
| `chrome/browser/abp/abp_network_capture.h/cc` | Per-tab in-memory buffer, CDP event handling, type filtering |
| `chrome/browser/abp/abp_network_database.h/cc` | SQLite schema, save/query/delete, regex filtering |
| `chrome/browser/abp/abp_curl_handler.h/cc` | SimpleURLLoader wrapper, cookie/session inheritance |

**Modified files:**

| File | Changes |
|------|---------|
| `abp_controller.h/cc` | Own AbpNetworkCapture per tab, wire CDP network events, new REST endpoints, curl dispatch |
| `abp_action_context.h/cc` | Parse `network` param, merge on wait, add network counts to response envelope |
| `abp_mcp_handler.h/cc` | `browser_network` and `browser_curl` tool handlers, network counts in MCP response |
| `abp_tool_builder.h/cc` | New tool schema definitions |
| `abp_types.h` | `CapturedRequest` struct, network config types |
| `BUILD.gn` | Add new source files |

## Resolved Ambiguities

| Question | Decision |
|----------|----------|
| **Redirects** | Grouped into single entry. Final response dominates. `redirect_chain` stores intermediate hops. |
| **CORS preflights** | Filtered out of buffer. Associated request marked with `cors_preflight: true`. |
| **Request body size** | `Network.enable` with `maxPostDataSize: 52428800` (50MB). |
| **browser_curl Origin/Referer** | Auto-populated from tab's current URL so request looks like page-triggered JS. |
| **Binary responses** | Base64-encoded in REST. Rendered as image/blob content blocks in MCP. |
| **Service workers** | Only capture final outbound request. SW-served responses skipped. |
| **Retroactive save tab_id** | Defaults to active tab if not specified. |
| **Buffer cap** | 1000 requests per tab. Oldest evicted FIFO when full. |
