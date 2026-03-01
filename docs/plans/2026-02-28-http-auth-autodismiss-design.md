# HTTP Auth Auto-Dismiss Design

## Problem

When a server returns HTTP 401/407, Chromium shows a native login dialog via `LoginHandler`. ABP doesn't intercept this, so the dialog is invisible to AI agents and blocks interaction. HTTP auth is a legacy mechanism and agents don't need to provide credentials — they just need to know it happened.

## Decision

Auto-dismiss HTTP auth dialogs and emit an event. No credential-providing endpoint. Follows the same pattern as non-geolocation permission auto-deny.

## Approach

Override at `ChromeContentBrowserClient::CreateLoginDelegate()` — the single interception point where all HTTP auth challenges arrive before any UI is created.

## Components

### New: `abp_login_delegate.h/cc`

Minimal `content::LoginDelegate` subclass:
- Constructor receives `net::AuthChallengeInfo`, a controller notification callback, and the `LoginAuthRequiredCallback`
- On construction: posts a task to cancel auth (`callback.Run(std::nullopt)`) and notify the controller
- Extracts from `AuthChallengeInfo`: `scheme`, `realm`, `challenger` (host:port), `is_proxy`, `path`

### Modified: `chrome_content_browser_client.cc`

In `CreateLoginDelegate()`, before the `http_auth_coordinator_` fallback:
- If not in test mode (`!HasSwitch("test-type")`), create and return `AbpLoginDelegate`
- Passes a weak callback to the ABP controller for event notification

### Modified: `abp_controller.h/cc`

New method `OnHttpAuthDismissed(scheme, realm, host, is_proxy, path)`:
- Calls `event_collector_->AddEvent("http_auth_dismissed", data)` if capturing
- Calls `history_controller_->RecordEvent(tab_id, "http_auth_dismissed", data)` for history

### Modified: `BUILD.gn`

Add `abp_login_delegate.h` and `abp_login_delegate.cc` to sources.

## Event Format

```json
{
  "type": "http_auth_dismissed",
  "data": {
    "scheme": "basic",
    "realm": "Restricted Area",
    "host": "example.com:443",
    "is_proxy": false,
    "path": "/protected"
  }
}
```

## What's Excluded

- No REST endpoint to provide credentials (can be added later if needed)
- No MCP tool changes
- No pending state on tabs
- No new MCP event type (uses existing event infrastructure)
