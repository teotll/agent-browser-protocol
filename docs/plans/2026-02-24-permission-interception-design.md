# Permission Request Interception Design

**Date:** 2026-02-24
**Status:** Approved

## Goal

Intercept all permission prompts before they reach the browser UI during ABP sessions. Geolocation permissions are surfaced as pending events for agent response via API. All other permissions are auto-denied with an event emitted. Optional mock geolocation coordinates can be provided without using CDP (avoiding detection).

## Architecture

```
Page requests permission
        │
        ▼
PermissionRequestManager::ShowPrompt()
        │
        ▼
AbpPermissionObserver::OnPromptAdded()
        │
        ├─ Geolocation? ──► Store pending, emit "permission_requested" event
        │                    Agent calls POST /api/v1/permissions/{id}/grant or /deny
        │                    → Accept()/Deny() via PermissionRequestManager (ABP action)
        │
        └─ Other? ─────────► Emit "permission_auto_denied" event
                             → Deny() immediately via PermissionRequestManager
```

Mock geolocation (independent of permission flow):

```
Agent calls POST /api/v1/geolocation    (not an ABP action, just sets state)
        │
        ▼
AbpLocationProvider::HandlePositionChanged()
        │
        ▼
navigator.geolocation sees mock coordinates
```

Uses `ContentBrowserClient::OverrideSystemLocationProvider()` — no CDP, completely native, undetectable.

## Design Decisions

- **Pure observer approach** over CDP `Browser.setPermission` to avoid adding detection surface. Permission grants go through the normal Chrome permission flow (same code path as real user clicking "Allow").
- **Mock geolocation via custom LocationProvider** instead of CDP `Emulation.setGeolocationOverride` — operates at the device service layer, indistinguishable from real OS geolocation at the JS API level.
- **Non-blocking permission events** — permission requests don't pause the action lifecycle. Events are emitted and agent responds later via separate API call.
- **Permission grant/deny is an ABP action** — goes through full resume/execute/wait/pause lifecycle so page reactions (geolocation callbacks, DOM changes) settle and screenshot is captured.
- **Set geolocation is NOT an action** — simple state setter, no lifecycle. Must be called before granting permission so coordinates are ready.
- **Geolocation only** for interactive handling. All other permission types auto-denied.

## New Classes

### AbpPermissionObserver

Observes `PermissionRequestManager` per tab. Implements `PermissionRequestManager::Observer`.

- Attached on tab creation, detached on tab close (same lifecycle as `AbpEventObserver`)
- `OnPromptAdded()`: inspects `Requests()` for permission type and origin
  - Geolocation → stores `PendingPermissionRequest` in controller, emits event
  - All others → calls `Deny()` immediately, emits `permission_auto_denied` event

### AbpLocationProvider

Custom geolocation provider implementing `device::LocationProvider` (modeled on `FakeLocationProvider`).

- Singleton instance returned via `ContentBrowserClient::OverrideSystemLocationProvider()`
- Holds current mock coordinates in memory
- `SetPosition(lat, lng, accuracy)` → calls `HandlePositionChanged()` to push to all listeners
- When no mock is set, returns `kPositionUnavailable` error — page sees "position unavailable", never real device location

## Pending State

Stored in `AbpController`:

```cpp
struct PendingPermissionRequest {
  std::string id;              // "perm_1", "perm_2", etc.
  std::string tab_id;
  std::string permission_type; // "geolocation"
  std::string origin;          // requesting origin URL
  int64_t requested_at_ms;     // virtual time
};

std::map<std::string, PendingPermissionRequest> pending_permissions_;
```

## Events

Geolocation permission requested (pending, waiting for agent):

```json
{
  "type": "permission_requested",
  "data": {
    "id": "perm_1",
    "tab_id": "...",
    "permission_type": "geolocation",
    "origin": "https://example.com"
  }
}
```

Non-geolocation permission auto-denied:

```json
{
  "type": "permission_auto_denied",
  "data": {
    "permission_type": "notifications",
    "origin": "https://example.com"
  }
}
```

## API Endpoints

### Permission Handling

| Method | Path | Type | Description |
|--------|------|------|-------------|
| GET | `/api/v1/permissions` | Not an action | List all pending permission requests |
| POST | `/api/v1/permissions/{id}/grant` | ABP action | Grant permission (resume → grant → wait → pause → screenshot) |
| POST | `/api/v1/permissions/{id}/deny` | ABP action | Deny permission (resume → deny → wait → pause → screenshot) |

### Mock Geolocation

| Method | Path | Type | Description |
|--------|------|------|-------------|
| POST | `/api/v1/geolocation` | Not an action | Set mock coordinates |
| DELETE | `/api/v1/geolocation` | Not an action | Clear mock (reverts to "unavailable") |

Set mock body:

```json
{
  "latitude": 40.7128,
  "longitude": -74.0060,
  "accuracy": 10.0
}
```

## MCP Tools

### respond_to_permission

ABP action tool. Grant or deny a pending permission request.

Description instructs agent: "If granting geolocation, call `set_geolocation` first to provide coordinates, then call this tool to grant."

### set_geolocation

Non-action tool. Sets mock geolocation coordinates.

Description: "Set mock geolocation coordinates. Call this BEFORE granting a geolocation permission so coordinates are available when the page receives the grant."

## Agent Workflow

```
1. Action triggers → event: "permission_requested" (geolocation)
2. Agent calls set_geolocation(lat, lng, accuracy)     ← just sets state
3. Agent calls respond_to_permission(id, "grant")       ← ABP action with full lifecycle
4. Page's geolocation callback fires with mock coords during the action's resume phase
5. Action captures screenshot + events after settle
```

## Error Handling & Edge Cases

- **Permission request while no action running**: Event stored as pending. Agent responds anytime via API.
- **Tab closes with pending permission**: Removed from `pending_permissions_` map in tab cleanup.
- **Multiple geolocation requests from same page**: `PermissionRequestManager` batches duplicates. Observer sees one prompt, Accept/Deny applies to all.
- **Agent never responds**: Permission stays pending indefinitely (same as dialogs). Page's promise stays unresolved.
- **Mock geolocation set before permission granted**: Coordinates ready. Once granted, `LocationProvider` delivers immediately.
- **No mock set, permission granted**: `LocationProvider` returns `kPositionUnavailable`. Page gets geolocation error, never real device location.

## Testing

- Browser test: permission prompt intercepted (no UI), event emitted, API grant/deny works
- Browser test: non-geolocation permissions auto-denied with event
- Browser test: mock geolocation delivers coordinates after permission grant
- Integration test (Python): end-to-end via REST API
