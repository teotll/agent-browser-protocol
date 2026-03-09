# Network Capture Improvements Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make network requests queryable immediately after any action without a save step, widen the default capture types, and update tool descriptions so agents understand the workflow.

**Architecture:** Add a `QueryBuffer()` method to `AbpNetworkCapture` that applies the same filters as the DB query. Modify `HandleNetworkQuery` in `AbpController` to query both buffer and DB, merge results with dedup. Update default type filter and MCP tool descriptions.

**Tech Stack:** C++ (Chromium), AbpNetworkCapture, AbpNetworkDatabase, AbpMcpHandler

---

### Task 1: Add `QueryBuffer` method to AbpNetworkCapture

**Files:**
- Modify: `chrome/browser/abp/abp_network_capture.h:64-68`
- Modify: `chrome/browser/abp/abp_network_capture.cc` (after `PassesTypeFilter`, ~line 113)

**Step 1: Add method declaration to header**

In `abp_network_capture.h`, add after the `GetRequests()` method (line 68):

```cpp
  // Query captured requests with filters, returning results as a JSON list.
  // Applies the same filtering logic as AbpNetworkDatabase::QueryFilter.
  // |tab_id| is stamped onto each result since the buffer doesn't store it.
  base::Value::List QueryBuffer(
      const AbpNetworkDatabase::QueryFilter& filter,
      const std::string& tab_id) const;
```

Add the include at the top of the header (after line 15):

```cpp
#include "chrome/browser/abp/abp_network_database.h"
```

**Step 2: Implement `QueryBuffer` in .cc**

Add at the end of `abp_network_capture.cc`, before the `RemoveAtIndex` method:

```cpp
base::Value::List AbpNetworkCapture::QueryBuffer(
    const AbpNetworkDatabase::QueryFilter& filter,
    const std::string& tab_id) const {
  base::Value::List results;

  for (const auto& req : requests_) {
    // Tag filter: buffer entries have no tag, so any tag filter excludes them.
    if (!filter.tag.empty()) {
      continue;
    }

    // Action ID filter: exact match.
    if (!filter.action_id.empty() && req.action_id != filter.action_id) {
      continue;
    }

    // Type filter: case-insensitive exact match.
    if (!filter.type.empty() &&
        !base::EqualsCaseInsensitiveASCII(filter.type, req.resource_type)) {
      continue;
    }

    // URL substring filters (LIKE semantics — case-insensitive contains).
    auto contains = [](const std::string& haystack,
                       const std::string& needle) {
      if (needle.empty()) return true;
      std::string h = base::ToLowerASCII(haystack);
      std::string n = base::ToLowerASCII(needle);
      return h.find(n) != std::string::npos;
    };

    if (!contains(req.url, filter.url_regex)) continue;
    if (!contains(req.url_hostname, filter.hostname_regex)) continue;
    if (!contains(req.url_path, filter.path_regex)) continue;
    if (!contains(req.url_query, filter.query_regex)) continue;
    if (!contains(req.method, filter.method_regex)) continue;
    if (!filter.status_regex.empty() &&
        !contains(std::to_string(req.status_code), filter.status_regex)) {
      continue;
    }

    // Build result dict — same shape as AbpNetworkDatabase::QueryRequestsOnDB.
    base::Value::Dict row;
    row.Set("request_id", req.request_id);
    row.Set("action_id", req.action_id);
    row.Set("tab_id", tab_id);
    // No "tag" field for buffer entries.
    row.Set("url", req.url);
    row.Set("url_hostname", req.url_hostname);
    row.Set("url_path", req.url_path);
    if (!req.url_query.empty()) {
      row.Set("url_query", req.url_query);
    }
    row.Set("method", req.method);

    if (filter.include_body) {
      if (!req.request_headers.empty()) {
        std::string headers_json;
        base::JSONWriter::Write(req.request_headers, &headers_json);
        row.Set("request_headers", headers_json);
      }
      if (!req.request_body.empty()) {
        row.Set("request_body", req.request_body);
      }
    }

    if (!req.resource_type.empty()) {
      row.Set("resource_type", req.resource_type);
    }
    row.Set("cors_preflight", req.cors_preflight);
    if (req.status_code != 0) {
      row.Set("status", req.status_code);
    }

    if (filter.include_body) {
      if (!req.response_headers.empty()) {
        std::string headers_json;
        base::JSONWriter::Write(req.response_headers, &headers_json);
        row.Set("response_headers", headers_json);
      }
      if (!req.response_body.empty()) {
        row.Set("response_body", req.response_body);
      }
      if (req.response_body_is_base64) {
        row.Set("response_body_encoding", "base64");
      }
    }

    if (!req.redirect_chain.empty()) {
      std::string chain_json;
      base::JSONWriter::Write(req.redirect_chain, &chain_json);
      row.Set("redirect_chain", chain_json);
    }

    if (req.started_at_ms != 0) {
      row.Set("started_at_ms", static_cast<double>(req.started_at_ms));
    }
    if (req.completed_at_ms != 0) {
      row.Set("completed_at_ms", static_cast<double>(req.completed_at_ms));
    }
    if (req.completed_at_ms != 0 && req.started_at_ms != 0) {
      row.Set("duration_ms",
              static_cast<double>(req.completed_at_ms - req.started_at_ms));
    }
    if (req.virtual_time_ms != 0) {
      row.Set("virtual_time_ms", static_cast<double>(req.virtual_time_ms));
    }

    results.Append(std::move(row));
  }

  return results;
}
```

**Step 3: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: BUILD SUCCESSFUL

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_network_capture.h chrome/browser/abp/abp_network_capture.cc
git commit -m "feat(abp): add QueryBuffer method to AbpNetworkCapture"
```

---

### Task 2: Merge buffer results into HandleNetworkQuery

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc:7368-7400` (HandleNetworkQuery)

**Step 1: Modify HandleNetworkQuery to query buffer first, then DB**

Replace the entire `HandleNetworkQuery` method with:

```cpp
void AbpController::HandleNetworkQuery(const std::string& query_string,
                                       ResponseCallback callback) {
  AbpNetworkDatabase::QueryFilter filter;
  filter.tag = GetQueryParam(query_string, "tag");
  filter.tab_id = GetQueryParam(query_string, "tab_id");
  filter.action_id = GetQueryParam(query_string, "action_id");
  filter.url_regex = GetQueryParam(query_string, "url");
  filter.hostname_regex = GetQueryParam(query_string, "hostname");
  filter.path_regex = GetQueryParam(query_string, "path");
  filter.query_regex = GetQueryParam(query_string, "query");
  filter.method_regex = GetQueryParam(query_string, "method");
  filter.status_regex = GetQueryParam(query_string, "status");
  filter.type = GetQueryParam(query_string, "type");
  std::string include_body_str = GetQueryParam(query_string, "include_body");
  filter.include_body = (include_body_str == "true" || include_body_str == "1");

  // Phase 1: Query in-memory buffer(s).
  base::Value::List buffer_results;
  std::set<std::pair<std::string, std::string>> seen;  // (tab_id, request_id)

  if (!filter.tab_id.empty()) {
    // Query specific tab's buffer.
    auto it = tab_states_.find(filter.tab_id);
    if (it != tab_states_.end() && it->second.network_capture) {
      buffer_results = it->second.network_capture->QueryBuffer(
          filter, filter.tab_id);
    }
  } else {
    // Query active tab's buffer.
    std::string active = GetActiveTabId();
    if (!active.empty()) {
      auto it = tab_states_.find(active);
      if (it != tab_states_.end() && it->second.network_capture) {
        buffer_results = it->second.network_capture->QueryBuffer(
            filter, active);
      }
    }
  }

  // Build dedup set from buffer results.
  for (const auto& entry : buffer_results) {
    if (entry.is_dict()) {
      const std::string* tid = entry.GetDict().FindString("tab_id");
      const std::string* rid = entry.GetDict().FindString("request_id");
      if (tid && rid) {
        seen.insert({*tid, *rid});
      }
    }
  }

  // Phase 2: Query DB if available, merge with dedup.
  if (!network_db_) {
    // No DB — return buffer results only.
    base::Value::Dict response;
    response.Set("requests", std::move(buffer_results));
    std::string json;
    base::JSONWriter::Write(response, &json);
    std::move(callback).Run(200, "application/json", std::move(json));
    return;
  }

  network_db_->QueryRequests(
      filter,
      base::BindOnce(
          [](ResponseCallback cb, base::Value::List buffer_results,
             std::set<std::pair<std::string, std::string>> seen,
             base::Value::List db_results) {
            // Append DB results that aren't already in buffer results.
            for (auto& entry : db_results) {
              if (entry.is_dict()) {
                const std::string* tid = entry.GetDict().FindString("tab_id");
                const std::string* rid =
                    entry.GetDict().FindString("request_id");
                if (tid && rid && seen.count({*tid, *rid})) {
                  continue;  // Already in buffer results.
                }
              }
              buffer_results.Append(std::move(entry));
            }

            base::Value::Dict response;
            response.Set("requests", std::move(buffer_results));
            std::string json;
            base::JSONWriter::Write(response, &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback), std::move(buffer_results), std::move(seen)));
}
```

**Step 2: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: BUILD SUCCESSFUL

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): merge buffer into network query results"
```

---

### Task 3: Change default type filter to include Document

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.cc:351`
- Modify: `chrome/browser/abp/abp_types.h:83`

**Step 1: Update the default in abp_action_context.cc**

At line 351, change:

```cpp
          types_set = {"XHR", "Fetch"};
```

to:

```cpp
          types_set = {"XHR", "Fetch", "Document"};
```

**Step 2: Update the default in abp_types.h**

At line 83, change:

```cpp
  std::set<std::string> types = {"XHR", "Fetch"};  // CDP resource types
```

to:

```cpp
  std::set<std::string> types = {"XHR", "Fetch", "Document"};  // CDP resource types
```

**Step 3: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: BUILD SUCCESSFUL

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_action_context.cc chrome/browser/abp/abp_types.h
git commit -m "feat(abp): add Document to default network capture types"
```

---

### Task 4: Update MCP tool descriptions

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:637-675` (browser_network description)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` (all `network_tag` descriptions)

**Step 1: Update browser_network tool description**

At lines 637-649, replace the Description string with:

```cpp
          .Description(
              "Query, save, or clear captured network requests. Every browser "
              "action automatically captures network requests (XHR, Fetch, and "
              "Document types by default).\n\n"
              "Captured requests from the most recent action are always "
              "available to query. Wait actions accumulate requests from prior "
              "actions; all other actions start with a fresh capture. To retain "
              "requests across actions, use save or network_tag on the "
              "action.\n\n"
              "action=\"query\": Search captured network requests. Supports "
              "filters on url, hostname, path, method, status, type, "
              "action_id, and tag. Use include_body=true to include request/"
              "response bodies.\n\n"
              "action=\"save\": Persist captured requests with a tag name so "
              "they remain available across future actions.\n\n"
              "action=\"clear\": Remove previously saved requests, optionally "
              "filtered by tag.")
```

**Step 2: Update all network_tag parameter descriptions**

Find all instances of:

```cpp
"Tag name to save captured network requests to the DB"
```

Replace with:

```cpp
"Tag name to auto-save this action's network requests. Saved requests "
"persist across actions and can be queried later by tag."
```

This appears at approximately these locations (use find-and-replace):
- Line 225 (inline JSON for browser_action)
- Line 331 (browser_scroll)
- Line 346 (browser_navigate)
- Line 376 (browser_screenshot GET)
- Line 400 (browser_wait)
- Line 432 (browser_javascript)
- Line 442 (browser_text)
- Line 587 (browser_slider)
- Line 604 (browser_clear_text)

**Step 3: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -5`
Expected: BUILD SUCCESSFUL

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc
git commit -m "docs(abp): update network tool descriptions for buffer query workflow"
```

---

### Task 5: Manual smoke test

**Step 1: Launch ABP**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Test buffer query without save**

```bash
# Navigate to a page
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Query network requests immediately — should return the Document request
# without needing to save first
curl 'http://localhost:8222/api/v1/network'
```

Expected: Response includes at least one request with `resource_type: "Document"` for example.com.

**Step 3: Test with tag filter excludes buffer**

```bash
curl 'http://localhost:8222/api/v1/network?tag=nonexistent'
```

Expected: Empty results (buffer entries have no tag).

**Step 4: Test save + query still works**

```bash
# Save buffer with a tag
curl -X POST http://localhost:8222/api/v1/network/save \
  -H "Content-Type: application/json" \
  -d '{"tag":"test"}'

# Query by tag — should return saved requests
curl 'http://localhost:8222/api/v1/network?tag=test'
```

Expected: Returns the saved requests with `tag: "test"`.

**Step 5: Test buffer clears on next non-wait action**

```bash
# Get tab ID from earlier response, then click somewhere
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":100}'

# Query again — buffer should be fresh (click is non-wait)
curl 'http://localhost:8222/api/v1/network'
```

Expected: Buffer results are from the click action only, not the original navigate. Saved requests with tag "test" still returned if no tag filter.

**Step 6: Test include_body on buffer query**

```bash
curl 'http://localhost:8222/api/v1/network?include_body=true&type=Document'
```

Expected: Results include `response_body` field with the HTML content.

**Step 7: Commit test results**

```bash
git commit --allow-empty -m "test(abp): verify network capture improvements smoke test"
```
