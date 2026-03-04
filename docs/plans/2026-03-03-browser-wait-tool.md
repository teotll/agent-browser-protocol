# browser_wait Tool Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a `browser_wait` MCP tool that waits for all in-flight same-site network requests to settle (5s max), backed by a persistent per-tab network tracker so requests from before the wait started are included.

**Architecture:** Three changes: (1) persistent same-site network tracking per tab updated on every CDP event; (2) `WaitForActionComplete` seeding from persistent set when `all_requests=true`; (3) new `WaitForNetwork()` controller method + REST route + MCP tool. Default timing also tightened from 250ms/750ms to 150ms/350ms for all other actions.

**Tech Stack:** C++17, Chromium base/, content/, CDP (Chrome DevTools Protocol)

---

### Task 1: Update default timing config

**Files:**
- Modify: `chrome/browser/abp/abp_config.h:36,40`
- Modify: `chrome/browser/abp/abp_action_context.h:83,91`

**Step 1: Change `AbpConfig::TimingConfig` defaults**

In `abp_config.h`, update the two defaults in `struct TimingConfig`:

```cpp
struct TimingConfig {
  // Phase 1: JS hook window before network snapshot
  base::TimeDelta min_wait = base::Milliseconds(150);   // was 250
  // Phase 2: How long to track in-flight requests
  base::TimeDelta tracking_timeout = base::Milliseconds(1000);
  // Phase 3: Settle after tracked requests complete
  base::TimeDelta post_settle = base::Milliseconds(350);  // was 750
};
```

**Step 2: Update matching defaults in `AbpActionContext::Options`**

In `abp_action_context.h`, update the defaults that parallel `TimingConfig`:

```cpp
// Phase 1: JS hook window.
base::TimeDelta min_wait_time = base::Milliseconds(150);  // was 250

// Phase 3: Settle time after tracked requests complete.
base::TimeDelta post_tracking_settle_time = base::Milliseconds(350);  // was 750
```

**Step 3: Build and verify**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: clean build, no errors.

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_config.h chrome/browser/abp/abp_action_context.h
git commit -m "feat(abp): tighten default wait timing to 150ms prewait / 350ms post-settle"
```

---

### Task 2: Add persistent network tracking fields

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:826-884` (TabState struct)
- Modify: `chrome/browser/abp/abp_controller.h:740-817` (ActionCompleteWaiter struct)

**Step 1: Add persistent fields to `TabState`**

In `abp_controller.h`, inside `struct TabState` (after the `tab_switched_to` field, around line 868):

```cpp
// Persistent same-site network tracking (always-on, updated by CDP events).
// Tracks in-flight request IDs regardless of whether a waiter is active.
// Cleared on main-frame navigation. Used to seed browser_wait's snapshot.
std::set<std::string> persistent_active_request_ids;
std::string persistent_page_domain;  // eTLD+1 of current page for filtering
```

**Step 2: Add `all_requests` field to `ActionCompleteWaiter`**

In `abp_controller.h`, inside `struct ActionCompleteWaiter` (after the `post_tracking_settled` field, around line 787):

```cpp
// If true, tracked_requests snapshot at prewait-end is seeded from the
// tab's persistent_active_request_ids (all in-flight, not just since wait start).
bool all_requests = false;
```

**Step 3: Add `all_requests` to `AbpActionContext::Options`**

In `abp_action_context.h`, inside `struct Options` (after `post_tracking_settle_time`):

```cpp
// If true, WaitForActionComplete seeds tracked_requests from the tab's
// persistent network tracker (all in-flight requests, not just new ones).
// Used by browser_wait to catch requests started before the wait began.
bool all_requests = false;
```

**Step 4: Build**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: clean build.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_action_context.h
git commit -m "feat(abp): add persistent network tracking fields and all_requests option"
```

---

### Task 3: Implement persistent network tracking in CDP event listener

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` — declare `OnPersistentNetworkEvent`
- Modify: `chrome/browser/abp/abp_controller.cc:3315-3355` — event listener lambda + new method

**Step 1: Declare `OnPersistentNetworkEvent` in header**

In `abp_controller.h`, near the other private event handler declarations (around line 983 where `MaybeStartMinWaitTimer` is declared):

```cpp
// Update persistent per-tab same-site network tracking.
// Called on every CDP event regardless of waiter state.
void OnPersistentNetworkEvent(const std::string& tab_id,
                              const std::string& method,
                              const base::Value::Dict& params);
```

**Step 2: Call `OnPersistentNetworkEvent` from the CDP event listener lambda**

In `abp_controller.cc`, inside the `SetEventListener` lambda (around line 3352, right before `controller->OnCdpEventForWait`):

```cpp
// Update persistent network tracking (always-on, before waiter check)
controller->OnPersistentNetworkEvent(tab, method, params);
// Forward to action-complete wait logic if a waiter is active
controller->OnCdpEventForWait(tab, method, params);
```

**Step 3: Implement `OnPersistentNetworkEvent`**

Add this method to `abp_controller.cc` near the other `OnCdpEvent*` methods (after `OnCdpEventForWait`):

```cpp
void AbpController::OnPersistentNetworkEvent(const std::string& tab_id,
                                              const std::string& method,
                                              const base::Value::Dict& params) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end()) return;
  TabState& tab_state = it->second;

  // On main-frame navigation: update domain and clear stale requests.
  if (method == "Page.frameNavigated") {
    const base::Value::Dict* frame = params.FindDict("frame");
    if (frame && !frame->FindString("parentId")) {  // main frame only
      const std::string* url_str = frame->FindString("url");
      if (url_str) {
        GURL url(*url_str);
        tab_state.persistent_page_domain =
            net::registry_controlled_domains::GetDomainAndRegistry(
                url,
                net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
      }
      tab_state.persistent_active_request_ids.clear();
    }
    return;
  }

  if (method == "Network.requestWillBeSent") {
    const std::string* request_id = params.FindString("requestId");
    if (!request_id) return;

    // Skip long-running connection types
    const std::string* type = params.FindStringByDottedPath("type");
    if (type && (*type == "WebSocket" || *type == "EventSource" ||
                 *type == "Ping" || *type == "Prefetch" ||
                 *type == "CSPViolationReport")) {
      return;
    }

    // Apply same-site domain filter
    if (!tab_state.persistent_page_domain.empty()) {
      const std::string* url_str = params.FindStringByDottedPath("request.url");
      if (url_str) {
        GURL url(*url_str);
        const std::string& domain = tab_state.persistent_page_domain;
        std::string host(url.host());
        bool same_site =
            (host == domain ||
             (host.size() > domain.size() &&
              host.compare(host.size() - domain.size(), domain.size(),
                           domain) == 0 &&
              host[host.size() - domain.size() - 1] == '.'));
        if (!same_site) return;
      }
    }
    tab_state.persistent_active_request_ids.insert(*request_id);

  } else if (method == "Network.loadingFinished" ||
             method == "Network.loadingFailed") {
    const std::string* request_id = params.FindString("requestId");
    if (request_id) {
      tab_state.persistent_active_request_ids.erase(*request_id);
    }
  }
}
```

**Step 4: Build**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: clean build.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): track same-site in-flight requests persistently per tab"
```

---

### Task 4: Thread `all_requests` through to `WaitForActionComplete` and seed snapshot

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:357-363` — `WaitForActionComplete` signature
- Modify: `chrome/browser/abp/abp_controller.cc:4642-4735` — `WaitForActionComplete` implementation
- Modify: `chrome/browser/abp/abp_controller.cc:4951-4996` — `OnMinWaitTimeElapsed`
- Modify: `chrome/browser/abp/abp_action_context.cc:555-562` — `DoWaitUntil`

**Step 1: Add `all_requests` param to `WaitForActionComplete` declaration**

In `abp_controller.h`, update the `WaitForActionComplete` signature (around line 357):

```cpp
void WaitForActionComplete(
    const std::string& tab_id,
    base::OnceClosure on_complete,
    base::TimeDelta min_wait_time = base::Milliseconds(150),
    base::TimeDelta request_tracking_timeout = base::Seconds(1),
    base::TimeDelta post_tracking_settle_time = base::Milliseconds(350),
    bool page_was_loaded_before_action = false,
    bool all_requests = false);
```

**Step 2: Accept `all_requests` in `WaitForActionComplete` and store in waiter**

In `abp_controller.cc`, update the function signature and store in waiter (after the existing `waiter->post_tracking_settle_time = post_tracking_settle_time;` line around 4665):

```cpp
void AbpController::WaitForActionComplete(
    const std::string& tab_id,
    base::OnceClosure on_complete,
    base::TimeDelta min_wait_time,
    base::TimeDelta request_tracking_timeout,
    base::TimeDelta post_tracking_settle_time,
    bool page_was_loaded_before_action,
    bool all_requests) {
  // ... existing code ...
  waiter->all_requests = all_requests;
  // ... rest of existing code ...
```

**Step 3: Seed snapshot from persistent set in `OnMinWaitTimeElapsed`**

In `abp_controller.cc`, replace the snapshot line in `OnMinWaitTimeElapsed` (around line 4961):

```cpp
// Phase 1 complete — take request tracking snapshot
if (waiter->all_requests) {
  // browser_wait mode: seed from persistent tracking (all in-flight same-site
  // requests regardless of when they started, not just since wait began).
  auto& tab_state = it->second;
  waiter->tracked_requests = tab_state.persistent_active_request_ids;
  // Also include any new requests seen during prewait
  waiter->tracked_requests.insert(waiter->active_request_ids.begin(),
                                  waiter->active_request_ids.end());
} else {
  waiter->tracked_requests = waiter->active_request_ids;
}
waiter->tracking_snapshot_taken = true;
```

**Step 4: Pass `options_.all_requests` from `AbpActionContext::DoWaitUntil`**

In `abp_action_context.cc`, update the `WaitForActionComplete` call in `DoWaitUntil` (around line 555):

```cpp
controller_->WaitForActionComplete(
    tab_id_,
    base::BindOnce(&AbpActionContext::OnWaitUntilComplete,
                   weak_factory_.GetWeakPtr()),
    options_.min_wait_time,
    options_.request_tracking_timeout,
    options_.post_tracking_settle_time,
    page_was_loaded_before_action_,
    options_.all_requests);
```

**Step 5: Build**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: clean build.

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc \
        chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): seed browser_wait snapshot from persistent in-flight request set"
```

---

### Task 5: Add `WaitForNetwork()` controller method and REST route

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h` — declare `WaitForNetwork()`
- Modify: `chrome/browser/abp/abp_controller.cc` — implement + route

**Step 1: Declare `WaitForNetwork` in header**

In `abp_controller.h`, near the other action method declarations (near `Wait()`):

```cpp
// Wait for all in-flight same-site network requests to settle.
// Uses all_requests=true so requests from before wait started are tracked.
// Timing: prewait=250ms, tracking_timeout=5s, post_settle=750ms.
void WaitForNetwork(const std::string& tab_id,
                    const base::Value::Dict& params,
                    ResponseCallback callback);
```

**Step 2: Implement `WaitForNetwork` in `abp_controller.cc`**

Add this method near the existing `Wait()` method (around line 2859):

```cpp
void AbpController::WaitForNetwork(const std::string& tab_id,
                                    const base::Value::Dict& params,
                                    ResponseCallback callback) {
  AbpActionContext::Options options;
  options.min_wait_time = base::Milliseconds(250);
  options.request_tracking_timeout = base::Seconds(5);
  options.post_tracking_settle_time = base::Milliseconds(750);
  options.all_requests = true;

  AbpActionContext::RunWithOptions(
      this, tab_id, "wait_for_network", params, options,
      // No action — goes directly to wait phase
      AbpActionContext::ActionCallback(),
      std::move(callback));
}
```

**Step 3: Add REST route `wait_for_network`**

In `abp_controller.cc`, inside the `segments.size() == 5` block of the route handler (around line 2010, after the `"wait"` handler):

```cpp
} else if (action == "wait_for_network") {
  WaitForNetwork(tab_id, params, std::move(callback));
```

**Step 4: Build**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: clean build.

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_controller.h chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add WaitForNetwork() controller method and REST route"
```

---

### Task 6: Add `browser_wait` MCP tool

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h:100-105` — add `CallBrowserWait` declaration
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` — tool definition, dispatch, implementation

**Step 1: Declare `CallBrowserWait` in header**

In `abp_mcp_handler.h`, after `CallBrowserClearText` (around line 100):

```cpp
void CallBrowserWait(const base::Value::Dict& args,
                     base::Value request_id,
                     ResponseWithHeadersCallback callback);
```

**Step 2: Add tool definition in `GetToolDefinitions()`**

In `abp_mcp_handler.cc`, add after the `browser_screenshot` tool (around line 326):

```cpp
// 4b. browser_wait — wait for network to settle
tools.Append(
    ToolBuilder("browser_wait")
        .Description(
            "Wait for all in-flight network requests to settle (up to 5 "
            "seconds). Resumes page execution, waits for the network to go "
            "idle, captures a screenshot, then re-pauses execution. Use "
            "this when the page is loading content after an action and you "
            "need to wait for it to finish.")
        .OptionalString("tab_id", "Target tab ID")
        .OptionalStringArrayEnum("markup",
            "Markup overlays to enable (none by default). clickable "
            "(green), typeable (orange), scrollable (purple dashed), "
            "grid (red coordinate grid), selected (blue, focused element)",
            {"clickable", "typeable", "scrollable", "grid", "selected"})
        .OptionalString("format", "Image format: png, webp, jpeg")
        .Build());
```

**Step 3: Add dispatch case**

In `abp_mcp_handler.cc`, inside `HandleToolsCall` (around line 865, after `"browser_clear_text"`):

```cpp
} else if (*name == "browser_wait") {
  CallBrowserWait(*args, std::move(request_id), std::move(callback));
```

**Step 4: Implement `CallBrowserWait`**

Add after `CallBrowserScreenshot` (around line 1069):

```cpp
void AbpMcpHandler::CallBrowserWait(const base::Value::Dict& args,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Build body with screenshot options (same shape as /screenshot)
  base::Value::Dict body_dict;
  base::Value::Dict screenshot_opts;
  if (const base::Value::List* markup = args.FindList("markup")) {
    screenshot_opts.Set("markup", markup->Clone());
  }
  if (const std::string* format = args.FindString("format")) {
    screenshot_opts.Set("format", *format);
  }
  body_dict.Set("screenshot", std::move(screenshot_opts));

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/wait_for_network", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 5: Build**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```
Expected: clean build.

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): add browser_wait MCP tool (5s network settle wait)"
```

---

### Task 7: Update tool count in comments/docs

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc` — update tool count comment if present
- Modify: `CLAUDE.md` — update "17 tools" to "18 tools" if mentioned

**Step 1: Check tool count references**

```bash
grep -n "17 tools\|16 tools\|tools at" chrome/browser/abp/abp_mcp_handler.cc CLAUDE.md
```

**Step 2: Update any found references** from the old count to the new count (18 tools).

**Step 3: Build one final time**

```bash
autoninja -C out/Default chrome 2>&1 | tail -5
```

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.cc CLAUDE.md
git commit -m "docs: update MCP tool count to 18 after adding browser_wait"
```

---

### Task 8: Manual smoke test

**Step 1: Launch ABP**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run &
sleep 3
```

**Step 2: Check browser_wait appears in MCP tools list**

```bash
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}' | python3 -m json.tool

curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | python3 -m json.tool | grep '"name"'
```

Expected: `"browser_wait"` appears in the list.

**Step 3: Test via REST**

```bash
# Get tab ID
TAB=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)['tabs'][0]['id'])")
echo "Tab: $TAB"

# Navigate somewhere with network activity
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}'

# Call wait_for_network
curl -s -X POST http://localhost:8222/api/v1/tabs/$TAB/wait_for_network \
  -H "Content-Type: application/json" \
  -d '{}' | python3 -c "import sys,json; d=json.load(sys.stdin); print('events:', [e['type'] for e in d.get('events',[])]); print('timing:', d.get('timing',{}))"
```

Expected: response with `timing` showing ~250ms+ wait, events may include network events.

**Step 4: Verify tool count**

```bash
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | python3 -c "import sys,json; tools=json.load(sys.stdin)['result']['tools']; print(f'{len(tools)} tools')"
```

Expected: `18 tools`
