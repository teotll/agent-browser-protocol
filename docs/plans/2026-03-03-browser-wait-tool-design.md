# browser_wait MCP Tool Design

**Date:** 2026-03-03

## Summary

Add a `browser_wait` MCP tool that waits for all in-flight same-site network requests to settle (up to 5 seconds), then returns a screenshot. Also update the default timing config to reduce prewait and post-settle for all other actions.

## Motivation

After a navigation or click, the page may have in-flight network requests that started before the previous action's 1-second tracking window closed. A follow-up `browser_wait` call should be able to observe and wait for ALL of these in-flight same-site requests to complete, not just the ones that happened to start during the new action's prewait.

## Key Design Decisions

### 1. Persistent Same-Site Request Tracking

**Problem:** CDP network events (`requestWillBeSent`, `loadingFinished`) are currently only tracked when an `ActionCompleteWaiter` is active. Between actions, in-flight requests are silently dropped.

**Solution:** Maintain a persistent `persistent_active_request_ids: std::set<std::string>` in `AbpTabState`. This set is updated by CDP network events regardless of whether a waiter is active.

**Filtering:** Same-site filtering is applied (same registrable domain + subdomains), matching the existing waiter logic. The domain is derived from `GetVisibleURL()` at tab-state level and updated on navigation.

**Navigation reset:** On `Page.frameNavigated` (main frame), clear the persistent set and update the page domain — same as the existing waiter behavior.

### 2. browser_wait Seeding

When `browser_wait` starts and the prewait elapses, the tracking snapshot is:

```
tracked_requests = persistent_active_request_ids ∪ requests_seen_during_prewait
```

This ensures requests in-flight from before the wait started are included in the 5-second tracking window.

The waiter flag `all_requests = true` enables this seeding behavior. Normal action waiters continue using only `requests_seen_since_wait_start`.

### 3. Timing

| Parameter       | browser_wait | Default (all other actions) |
|-----------------|-------------|-----------------------------|
| prewait         | 250ms        | **150ms** (was 250ms)       |
| tracking timeout| **5s** (new) | 1s                          |
| post-settle     | 750ms        | **350ms** (was 750ms)       |

`browser_wait` uses the old defaults for prewait/post-settle to give more room for JS to fire and pages to fully settle. Only the tracking timeout is increased.

### 4. REST Endpoint

New route: `POST /api/v1/tabs/{id}/wait_for_network`

Runs `AbpActionContext` with:
- No action callback (goes directly to wait phase)
- `min_wait=250ms`, `tracking_timeout=5s`, `post_settle=750ms`
- `all_requests=true` (seeds from persistent set)
- Returns full action envelope (before/after screenshots, events, timing)

### 5. MCP Tool Definition

Tool name: `browser_wait`

Parameters:
- `tab_id` (optional): Target tab ID
- `markup` (optional): Markup overlays to enable (same as browser_screenshot)
- `format` (optional): Image format (png, webp, jpeg)

Description: "Wait for all in-flight network requests to settle (up to 5 seconds). Resumes page execution, waits for the network to go idle, captures a screenshot, then re-pauses. Use this when the page is loading content after an action and you need to wait for it to finish."

## Files Changed

1. **`chrome/browser/abp/abp_config.h`** — Update `TimingConfig` defaults: `min_wait=150ms`, `post_settle=350ms`

2. **`chrome/browser/abp/abp_controller.h`** — Add `persistent_active_request_ids` + `persistent_page_domain` to `AbpTabState`; add `all_requests` field to `ActionCompleteWaiter`; declare `WaitForNetwork()`

3. **`chrome/browser/abp/abp_controller.cc`**:
   - Update CDP event listener to maintain `persistent_active_request_ids` (with same-site filtering using `persistent_page_domain`)
   - On `Page.frameNavigated`: clear persistent set and update `persistent_page_domain`
   - In `WaitForActionComplete`: when `all_requests=true`, seed `waiter->active_request_ids` from `persistent_active_request_ids` at prewait-end (in `OnMinWaitTimeElapsed`)
   - Add `WaitForNetwork()` method — `AbpActionContext::RunWithOptions` with custom timing + `all_requests=true`
   - Add route handler for `POST /api/v1/tabs/{id}/wait_for_network`

4. **`chrome/browser/abp/abp_mcp_handler.h`** — Declare `CallBrowserWait()`

5. **`chrome/browser/abp/abp_mcp_handler.cc`**:
   - Add `browser_wait` tool definition (after `browser_screenshot`)
   - Add `else if (*name == "browser_wait")` dispatch
   - Implement `CallBrowserWait()` — calls `POST /api/v1/tabs/{id}/wait_for_network`

## Behavior

```
browser_wait lifecycle:
  1. [before screenshot from frozen buffer]
  2. Resume JS execution
  3. Prewait 250ms (JS fires handlers, new requests start)
  4. Snapshot: tracked_requests = persistent_active_requests ∪ new_requests_during_prewait
  5. Wait up to 5s for all tracked requests to complete
  6. Post-settle 750ms
  7. Capture after screenshot
  8. Pause JS execution
  9. Return action envelope
```

## Non-Goals

- No tracking of cross-site (third-party) requests — consistent with existing behavior
- No timeout increase for the outer action timeout (still 30s max)
- No changes to the existing duration-based `POST /api/v1/tabs/{id}/wait` endpoint
