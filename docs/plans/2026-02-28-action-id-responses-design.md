# Action ID in Responses

## Problem

ABP action responses don't include an action identifier. Agents cannot correlate responses with history records, track action sequences, or reference specific actions in error reports.

Currently, action IDs are SQLite auto-incremented integers assigned *after* the response is sent — the ID doesn't exist at response time.

## Design

### ID Format

6-character uppercase alphanumeric string (A-Z, 0-9). Generated randomly with `base::RandInt`. ~2.18 billion possible values (36^6), no collision checking needed for session scope.

Examples: `A7K3MX`, `9BRT2F`, `WN4P8L`

### ID Generation

Generated in the `AbpActionContext` constructor via a static `GenerateActionId()` helper. Available from the moment the action starts, regardless of outcome.

```cpp
static std::string GenerateActionId();
// Uses base::RandInt to pick from "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
```

### Response Format

Top-level field in the action response envelope:

```json
{
  "action_id": "A7K3MX",
  "result": { ... },
  "screenshot_before": { ... },
  "screenshot_after": { ... },
  "timing": { ... },
  "events": [ ... ]
}
```

Error responses also include the action_id:

```json
{
  "action_id": "A7K3MX",
  "error": "tab_not_found",
  "message": "No tab with id ..."
}
```

### Scope

Only action endpoints that use `AbpActionContext` (click, type, navigate, screenshot, execute, scroll, etc.). Simple reads (GET /tabs, GET /browser/status) do not return action_ids.

### History Database Changes

- `ActionRecord.id`: `int64_t` -> `std::string`
- Database column: `INTEGER PRIMARY KEY AUTOINCREMENT` -> `TEXT PRIMARY KEY`
- `InsertActionOnDB()` writes the pre-generated string ID
- History API responses: `"id": 42` -> `"id": "A7K3MX"`
- History endpoints accept string IDs

Breaking change to history API. No migration needed — fresh database per session.

### MCP Handler

No changes. The action_id is part of the action response JSON, which the MCP handler already serializes as a text content block. Flows through automatically.

## Files Changed

- `chrome/browser/abp/abp_action_context.h` — add `action_id_` member, `GenerateActionId()` static method
- `chrome/browser/abp/abp_action_context.cc` — generate ID in constructor, include in `SendResponse()`
- `chrome/browser/abp/abp_history_database.h` — change `ActionRecord.id` to `std::string`
- `chrome/browser/abp/abp_history_database.cc` — change schema and insert logic
- `chrome/browser/abp/abp_history_controller.h` — update method signatures for string IDs
- `chrome/browser/abp/abp_history_controller.cc` — update query/response handling for string IDs
- `chrome/browser/abp/abp_controller.cc` — update any history ID references
