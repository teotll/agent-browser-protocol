# Network Capture Improvements Design

## Problem

During API discovery workflows (e.g., reverse-engineering Zillow's search API), the current network capture tools have three friction points:

1. **Query requires a save step.** `browser_network query` only searches the database. To see what network requests an action made, the agent must first call `save`, then `query` — two steps for a basic "what just happened?" question.

2. **Default type filter is too narrow.** The default `{"XHR", "Fetch"}` misses `Document` requests, which carry critical data on SSR sites (e.g., Next.js `__NEXT_DATA__`). This silently discards the most useful requests for API discovery.

3. **Tool descriptions don't explain the workflow.** The agent doesn't know that requests are captured per-action, that wait preserves them, or that it can query immediately after an action. The description says "stored in the session database" which implies save-first.

## Changes

### 1. Query merges buffer and DB results

`browser_network action=query` searches both the in-memory per-tab buffer and the saved database, returning a merged, deduplicated result set.

- Same filters apply to both sources (url, hostname, path, method, status, type, action_id, tag, include_body)
- Deduplication by `(tab_id, request_id)` — buffer wins for freshness
- Buffer entries have no `tag`, so a tag filter naturally excludes them
- Same response format as today

### 2. Default type filter: `{"XHR", "Fetch", "Document"}`

Change the hardcoded default in `abp_action_context.cc` from `{"XHR", "Fetch"}` to `{"XHR", "Fetch", "Document"}`.

### 3. Updated MCP tool descriptions

**`browser_network` tool:**

> Query, save, or clear captured network requests. Every browser action automatically captures network requests (XHR, Fetch, and Document types by default).
>
> Captured requests from the most recent action are always available to query. Wait actions accumulate requests from prior actions; all other actions start with a fresh capture. To retain requests across actions, use `save` or `network_tag` on the action.
>
> action="query": Search captured network requests. Supports filters on url, hostname, path, method, status, type, action_id, and tag. Use include_body=true to include request/response bodies.
>
> action="save": Persist captured requests with a tag name so they remain available across future actions.
>
> action="clear": Remove previously saved requests, optionally filtered by tag.

**`network_tag` parameter on action tools:**

> Tag name to auto-save this action's network requests. Saved requests persist across actions and can be queried later by tag.

### 4. No behavior changes

- Buffer clearing: non-wait actions clear, wait actions preserve (unchanged)
- Buffer size: 1000-entry FIFO eviction (unchanged)
- `save` and `clear` semantics (unchanged)
- DB schema (unchanged)
- `browser_curl` (unchanged)

## Implementation

### Files to modify

| File | Change |
|------|--------|
| `abp_action_context.cc` | Default type set: `{"XHR","Fetch"}` → `{"XHR","Fetch","Document"}` |
| `abp_network_capture.h/cc` | Add `QueryRequests(filter)` method that applies same filters as DB query (url regex, type, method, status, action_id, include_body) to the in-memory buffer |
| `abp_controller.cc` | `HandleNetworkQuery`: query buffer via new method, query DB, merge results with dedup by `(tab_id, request_id)` |
| `abp_mcp_handler.cc` | Update `browser_network` tool description and `network_tag` parameter descriptions |

### Buffer query implementation

Add to `AbpNetworkCapture`:

```cpp
// Apply filters to the in-memory buffer and return matching requests.
// Uses the same filter structure as AbpNetworkDatabase::QueryFilter.
std::vector<abp::CapturedRequest> QueryRequests(
    const AbpNetworkDatabase::QueryFilter& filter) const;
```

This method iterates the buffer, applying:
- `url` / `hostname` / `path` / `query` — substring match (LIKE semantics)
- `method` / `status` — substring match
- `type` — exact match (case-insensitive)
- `action_id` — exact match
- `tag` — buffer entries have no tag, so a non-empty tag filter returns empty
- `include_body` — controls whether headers/bodies are included in results

### Merge logic in HandleNetworkQuery

```
1. Query DB with filter → db_results
2. Query buffer(s) with filter → buffer_results
   - If filter has tab_id: query that tab's buffer only
   - If no tab_id: query active tab's buffer
3. Build seen set from buffer_results: {(tab_id, request_id)}
4. Append db_results where (tab_id, request_id) not in seen set
5. Return merged list
```

Buffer results come first (most recent/fresh), DB results appended for historical data.
