# Action ID in Responses Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Return a unique 6-character uppercase alphanumeric `action_id` in every ABP action response so agents can correlate responses with history, track sequences, and reference actions in debugging.

**Architecture:** Generate the ID in `AbpActionContext` constructor. Include it as a top-level field in both success and error response envelopes. Change the history database to use this string ID as primary key, with SQLite's implicit `rowid` for pagination ordering.

**Tech Stack:** C++ (Chromium), SQLite, `base::RandInt`

---

### Task 1: Add action_id generation to AbpActionContext

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h:7-10` (add include)
- Modify: `chrome/browser/abp/abp_action_context.h:137-138` (add accessor)
- Modify: `chrome/browser/abp/abp_action_context.h:153-155` (add static method declaration)
- Modify: `chrome/browser/abp/abp_action_context.h:201-204` (add member)
- Modify: `chrome/browser/abp/abp_action_context.cc:1-17` (add include)
- Modify: `chrome/browser/abp/abp_action_context.cc:137-150` (generate in constructor)

**Step 1: Add GenerateActionId static method and action_id_ member to header**

In `abp_action_context.h`, add the public accessor and static method:

```cpp
// After line 138 (tab_id accessor):
  // Access to action_id
  const std::string& action_id() const { return action_id_; }

// After line 155 (~AbpActionContext):
  static std::string GenerateActionId();
```

Add the member variable:

```cpp
// After line 204 (action_type_):
  std::string action_id_;
```

**Step 2: Implement GenerateActionId and call in constructor**

In `abp_action_context.cc`, add the include:

```cpp
#include "base/rand_util.h"
```

Add the static method (after the anonymous namespace, before `Run()`):

```cpp
// static
std::string AbpActionContext::GenerateActionId() {
  static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string id(6, '\0');
  for (int i = 0; i < 6; ++i) {
    id[i] = kChars[base::RandInt(0, 35)];
  }
  return id;
}
```

In the constructor (line 137-150), add `action_id_` initialization:

```cpp
AbpActionContext::AbpActionContext(...)
    : controller_(controller->GetWeakPtr()),
      tab_id_(tab_id),
      action_id_(GenerateActionId()),   // ADD THIS LINE
      action_type_(action_type),
      ...
```

**Step 3: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Builds successfully

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_action_context.h chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): add action_id generation to AbpActionContext"
```

---

### Task 2: Include action_id in success and error responses

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.cc:741-864` (SendResponse — add action_id to envelope)
- Modify: `chrome/browser/abp/abp_action_context.cc:866-892` (SendErrorResponse — add action_id)
- Modify: `chrome/browser/abp/abp_action_context.cc:894-918` (Fail — add action_id)

**Step 1: Add action_id to success response envelope**

In `SendResponse()` at line 754, add action_id as first field:

```cpp
  // Build full response envelope
  base::Value::Dict envelope;

  // 0. Add action ID
  envelope.Set("action_id", action_id_);

  // 1. Add action result
  envelope.Set("result", std::move(result_));
```

**Step 2: Add action_id to error responses**

In `SendErrorResponse()` (line 866-892), replace the call to `controller_->SendError()` with a custom error envelope that includes action_id:

```cpp
void AbpActionContext::SendErrorResponse(int status,
                                         const std::string& error_code,
                                         const std::string& error_message) {
  action_timeout_timer_.Stop();
  if (!IsCurrentAction()) {
    return;
  }
  if (!response_callback_) {
    ReleaseDeterministicSlot();
    prevent_destroy_ = nullptr;
    return;
  }

  // Record the error in history if we have enough context
  if (!has_error_) {
    has_error_ = true;
    error_code_ = error_code;
    error_message_ = error_message;
    RecordHistory(false, error_code, error_message);
  }

  // Build error response with action_id
  base::Value::Dict envelope;
  envelope.Set("action_id", action_id_);
  envelope.Set("error", error_code);
  envelope.Set("message", error_message);
  controller_->SendJson(status, base::Value(std::move(envelope)),
                         std::move(response_callback_));

  ReleaseDeterministicSlot();
  // Clear self-reference to allow destruction
  prevent_destroy_ = nullptr;
}
```

In `Fail()` (line 894-918), same change — replace `controller_->SendError()` with envelope including action_id:

```cpp
void AbpActionContext::Fail(int http_status,
                            const std::string& error_code,
                            const std::string& error_message) {
  action_timeout_timer_.Stop();

  if (!IsCurrentAction()) {
    ReleaseDeterministicSlot();
    prevent_destroy_ = nullptr;
    return;
  }

  has_error_ = true;
  error_code_ = error_code;
  error_message_ = error_message;

  RecordHistory(false, error_code, error_message);

  if (response_callback_ && controller_) {
    base::Value::Dict envelope;
    envelope.Set("action_id", action_id_);
    envelope.Set("error", error_code);
    envelope.Set("message", error_message);
    controller_->SendJson(http_status, base::Value(std::move(envelope)),
                           std::move(response_callback_));
  }

  ReleaseDeterministicSlot();
  prevent_destroy_ = nullptr;
}
```

**Step 3: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Builds successfully

**Step 4: Commit**

```bash
git add chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): include action_id in success and error responses"
```

---

### Task 3: Change history database to use string action IDs

**Files:**
- Modify: `chrome/browser/abp/abp_history_database.h:42-64` (ActionRecord.id type)
- Modify: `chrome/browser/abp/abp_history_database.h:84-99` (ActionQueryFilter — keep cursor as int64 for rowid)
- Modify: `chrome/browser/abp/abp_history_database.h:184` (InsertActionCallback)
- Modify: `chrome/browser/abp/abp_history_database.h:212-213` (GetAction signature)
- Modify: `chrome/browser/abp/abp_history_database.h:248-249` (GetActionOnDB signature)
- Modify: `chrome/browser/abp/abp_history_database.cc:83-99` (kCreateActionsTable schema)
- Modify: `chrome/browser/abp/abp_history_database.cc:464-496` (InsertActionOnDB — include id in INSERT)
- Modify: `chrome/browser/abp/abp_history_database.cc:498-544` (GetActionOnDB — string id)
- Modify: `chrome/browser/abp/abp_history_database.cc:557-668` (GetActionsOnDB — use rowid for pagination, read string id)

**Step 1: Change ActionRecord.id to std::string**

In `abp_history_database.h` line 51:

```cpp
  std::string id;  // was: int64_t id = 0;
```

**Step 2: Change InsertActionCallback and GetAction signatures**

In `abp_history_database.h`:

```cpp
  // line 184 — no longer returns id (it's pre-set in record)
  using InsertActionCallback = base::OnceCallback<void()>;

  // line 213 — accepts string id
  void GetAction(const std::string& action_id, ActionCallback callback);

  // line 249 — accepts string id
  std::optional<ActionRecord> GetActionOnDB(const std::string& action_id);
```

**Step 3: Change kCreateActionsTable schema**

In `abp_history_database.cc` lines 83-99:

```cpp
constexpr char kCreateActionsTable[] = R"(
  CREATE TABLE IF NOT EXISTS actions (
    id TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES sessions(id),
    tab_id TEXT,
    action_type TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    duration_ms INTEGER,
    params TEXT,
    result TEXT,
    success INTEGER NOT NULL,
    error_code TEXT,
    error_message TEXT,
    screenshot_before_path TEXT,
    screenshot_after_path TEXT
  )
)";
```

**Step 4: Update InsertActionOnDB to include the string id**

In `abp_history_database.cc`, change `InsertAction` to use `VoidCallback`:

```cpp
void AbpHistoryDatabase::InsertAction(const ActionRecord& action,
                                      InsertActionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::InsertActionOnDB,
                     base::Unretained(this), action),
      std::move(callback));
}
```

Change `InsertActionOnDB` return type to `void` and include `id` in INSERT:

```cpp
void AbpHistoryDatabase::InsertActionOnDB(ActionRecord action) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT INTO actions (id, session_id, tab_id, action_type, timestamp, "
      "duration_ms, params, result, success, error_code, error_message, "
      "screenshot_before_path, screenshot_after_path) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

  stmt.BindString(0, action.id);
  stmt.BindString(1, action.session_id);
  stmt.BindString(2, action.tab_id);
  stmt.BindString(3, action.action_type);
  stmt.BindInt64(4, action.timestamp);
  stmt.BindInt64(5, action.duration_ms);
  stmt.BindString(6, action.params_json);
  stmt.BindString(7, action.result_json);
  stmt.BindInt(8, action.success ? 1 : 0);
  stmt.BindString(9, action.error_code);
  stmt.BindString(10, action.error_message);
  stmt.BindString(11, action.screenshot_before_path);
  stmt.BindString(12, action.screenshot_after_path);

  if (!stmt.Run()) {
    LOG(ERROR) << "ABP: Failed to insert action: " << action.id;
  }
}
```

**Step 5: Update GetActionOnDB to use string id**

```cpp
void AbpHistoryDatabase::GetAction(const std::string& action_id,
                                    ActionCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(ui_sequence_checker_);

  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpHistoryDatabase::GetActionOnDB, base::Unretained(this),
                     action_id),
      std::move(callback));
}

std::optional<ActionRecord> AbpHistoryDatabase::GetActionOnDB(
    const std::string& action_id) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return std::nullopt;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "SELECT id, session_id, tab_id, action_type, timestamp, duration_ms, "
      "params, result, success, error_code, error_message, "
      "screenshot_before_path, screenshot_after_path "
      "FROM actions WHERE id = ?"));

  stmt.BindString(0, action_id);

  if (!stmt.Step()) {
    return std::nullopt;
  }

  ActionRecord record;
  record.id = stmt.ColumnString(0);
  record.session_id = stmt.ColumnString(1);
  record.tab_id = stmt.ColumnString(2);
  record.action_type = stmt.ColumnString(3);
  record.timestamp = stmt.ColumnInt64(4);
  record.duration_ms = stmt.ColumnInt64(5);
  record.params_json = stmt.ColumnString(6);
  record.result_json = stmt.ColumnString(7);
  record.success = stmt.ColumnInt(8) != 0;
  record.error_code = stmt.ColumnString(9);
  record.error_message = stmt.ColumnString(10);
  record.screenshot_before_path = stmt.ColumnString(11);
  record.screenshot_after_path = stmt.ColumnString(12);

  return record;
}
```

**Step 6: Update GetActionsOnDB to use rowid for pagination, string id for record**

The `cursor` field in `ActionQueryFilter` stays as `int64_t` — it now refers to SQLite's implicit `rowid` for ordering. The `id` field in the record is the string action ID.

```cpp
ActionsResult AbpHistoryDatabase::GetActionsOnDB(
    const ActionQueryFilter& filter) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  ActionsResult result;

  if (!db_) {
    return result;
  }

  // Build query with filters — use rowid for ordering/pagination
  std::string query =
      "SELECT rowid, id, session_id, tab_id, action_type, timestamp, duration_ms, "
      "params, result, success, error_code, error_message, "
      "screenshot_before_path, screenshot_after_path "
      "FROM actions WHERE 1=1";

  std::vector<std::string> string_binds;
  std::vector<int64_t> int_binds;

  if (!filter.session_id.empty()) {
    query += " AND session_id = ?";
    string_binds.push_back(filter.session_id);
  }
  if (!filter.tab_id.empty()) {
    query += " AND tab_id = ?";
    string_binds.push_back(filter.tab_id);
  }
  if (!filter.action_type.empty()) {
    query += " AND action_type = ?";
    string_binds.push_back(filter.action_type);
  }
  if (filter.success.has_value()) {
    query += " AND success = ?";
    int_binds.push_back(*filter.success ? 1 : 0);
  }
  if (filter.start_time > 0) {
    query += " AND timestamp >= ?";
    int_binds.push_back(filter.start_time);
  }
  if (filter.end_time > 0) {
    query += " AND timestamp <= ?";
    int_binds.push_back(filter.end_time);
  }

  if (filter.cursor > 0) {
    if (filter.forward) {
      query += " AND rowid > ?";
    } else {
      query += " AND rowid < ?";
    }
    int_binds.push_back(filter.cursor);
  }

  if (filter.forward) {
    query += " ORDER BY rowid ASC";
  } else {
    query += " ORDER BY rowid DESC";
  }

  query += " LIMIT ?";
  int_binds.push_back(filter.limit + 1);

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int param_idx = 0;
  for (const auto& s : string_binds) {
    stmt.BindString(param_idx++, s);
  }
  for (int64_t i : int_binds) {
    stmt.BindInt64(param_idx++, i);
  }

  std::vector<int64_t> rowids;  // track rowids for cursor
  while (stmt.Step()) {
    if (static_cast<int>(result.actions.size()) >= filter.limit) {
      result.has_more = true;
      break;
    }

    int64_t rowid = stmt.ColumnInt64(0);
    rowids.push_back(rowid);

    ActionRecord record;
    record.id = stmt.ColumnString(1);
    record.session_id = stmt.ColumnString(2);
    record.tab_id = stmt.ColumnString(3);
    record.action_type = stmt.ColumnString(4);
    record.timestamp = stmt.ColumnInt64(5);
    record.duration_ms = stmt.ColumnInt64(6);
    record.params_json = stmt.ColumnString(7);
    record.result_json = stmt.ColumnString(8);
    record.success = stmt.ColumnInt(9) != 0;
    record.error_code = stmt.ColumnString(10);
    record.error_message = stmt.ColumnString(11);
    record.screenshot_before_path = stmt.ColumnString(12);
    record.screenshot_after_path = stmt.ColumnString(13);
    result.actions.push_back(std::move(record));
  }

  // Set cursors using rowid
  if (!rowids.empty()) {
    if (filter.forward) {
      result.prev_cursor = base::NumberToString(rowids.front());
      if (result.has_more) {
        result.next_cursor = base::NumberToString(rowids.back());
      }
    } else {
      if (result.has_more) {
        result.prev_cursor = base::NumberToString(rowids.back());
      }
      result.next_cursor = base::NumberToString(rowids.front());
    }
  }

  return result;
}
```

**Step 7: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Builds successfully

**Step 8: Commit**

```bash
git add chrome/browser/abp/abp_history_database.h chrome/browser/abp/abp_history_database.cc
git commit -m "feat(abp): change history actions to use string action IDs"
```

---

### Task 4: Update history controller for string action IDs

**Files:**
- Modify: `chrome/browser/abp/abp_history_controller.h:103` (HandleGetAction signature)
- Modify: `chrome/browser/abp/abp_history_controller.h:104-106` (HandleGetActionScreenshot signature)
- Modify: `chrome/browser/abp/abp_history_controller.cc:51-53` (ActionRecordToJson — string id)
- Modify: `chrome/browser/abp/abp_history_controller.cc:207-243` (RecordAction — accept action_id param)
- Modify: `chrome/browser/abp/abp_history_controller.cc:387-420` (HandleRequest — parse string action ID from URL)
- Modify: `chrome/browser/abp/abp_history_controller.cc:589-594` (HandleGetAction — string)
- Modify: `chrome/browser/abp/abp_history_controller.cc:596-604` (HandleGetActionScreenshot — string)

**Step 1: Update ActionRecordToJson**

In `abp_history_controller.cc` line 53, change from int cast to string:

```cpp
  dict.Set("id", action.id);  // was: dict.Set("id", static_cast<int>(action.id));
```

**Step 2: Update RecordAction to accept action_id parameter**

In `abp_history_controller.h`, add `action_id` parameter to `RecordAction`:

```cpp
  void RecordAction(const std::string& action_id,   // ADD THIS
                    const std::string& tab_id,
                    const std::string& action_type,
                    const base::Value::Dict& params,
                    const base::Value* result,
                    bool success,
                    const std::string& error_code,
                    const std::string& error_message,
                    int64_t start_time,
                    int64_t duration_ms,
                    const std::string& screenshot_before_path = "",
                    const std::string& screenshot_after_path = "");
```

In `abp_history_controller.cc` `RecordAction()`, set `action.id`:

```cpp
void AbpHistoryController::RecordAction(const std::string& action_id,
                                        const std::string& tab_id,
                                        ...) {
  ...
  ActionRecord action;
  action.id = action_id;         // ADD THIS
  action.session_id = session_id_;
  ...
  database_->InsertAction(action, base::DoNothing());
}
```

**Step 3: Update HandleRequest to parse string action IDs**

In `abp_history_controller.cc` lines 387-420, change the action ID parsing from `base::StringToInt64` to just using the string directly:

```cpp
    if (segments.size() >= 5) {
      const std::string& action_id = segments[4];

      if (segments.size() == 5) {
        if (method == "GET") {
          HandleGetAction(action_id, std::move(callback));
        } else {
          ...
        }
        return;
      }

      if (segments.size() == 6 && segments[5] == "screenshot") {
        if (method == "GET") {
          auto params = ParseQueryString(query);
          std::string type = "after";
          auto it = params.find("type");
          if (it != params.end()) {
            type = it->second;
          }
          HandleGetActionScreenshot(action_id, type, std::move(callback));
        } else {
          ...
        }
        return;
      }
    }
```

**Step 4: Update HandleGetAction and HandleGetActionScreenshot signatures**

In `abp_history_controller.h`:

```cpp
  void HandleGetAction(const std::string& action_id, ResponseCallback callback);
  void HandleGetActionScreenshot(const std::string& action_id,
                                 const std::string& type,
                                 ResponseCallback callback);
```

In `abp_history_controller.cc`:

```cpp
void AbpHistoryController::HandleGetAction(const std::string& action_id,
                                           ResponseCallback callback) {
  database_->GetAction(
      action_id, base::BindOnce(&AbpHistoryController::OnActionResult,
                                weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpHistoryController::HandleGetActionScreenshot(
    const std::string& action_id,
    const std::string& type,
    ResponseCallback callback) {
  database_->GetAction(
      action_id, base::BindOnce(&AbpHistoryController::OnActionForScreenshot,
                                weak_factory_.GetWeakPtr(), std::move(callback),
                                type));
}
```

**Step 5: Build and verify compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Builds successfully

**Step 6: Commit**

```bash
git add chrome/browser/abp/abp_history_controller.h chrome/browser/abp/abp_history_controller.cc
git commit -m "feat(abp): update history controller for string action IDs"
```

---

### Task 5: Update AbpActionContext::RecordHistory to pass action_id

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.cc:719-734` (RecordHistory — pass action_id_)

**Step 1: Update RecordHistory call to include action_id_**

In `abp_action_context.cc`, update `RecordHistory()`:

```cpp
void AbpActionContext::RecordHistory(bool success,
                                     const std::string& error_code,
                                     const std::string& error_message) {
  if (!controller_->history_controller_) {
    return;
  }

  int64_t end_time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  int64_t duration_ms = end_time_ms - start_time_ms_;

  base::Value result_value(result_.Clone());
  controller_->history_controller_->RecordAction(
      action_id_,     // ADD THIS as first argument
      tab_id_, action_type_, params_, &result_value, success, error_code,
      error_message, start_time_ms_, duration_ms, screenshot_before_path_,
      screenshot_after_path_);
}
```

**Step 2: Build and verify full compilation**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Builds successfully with no errors

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_action_context.cc
git commit -m "feat(abp): pass action_id through to history recording"
```

---

### Task 6: Verify end-to-end with manual test

**Step 1: Launch ABP and test action response**

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/test_action_id --no-first-run &
sleep 3
```

**Step 2: Create a tab and verify action_id in response**

```bash
# Create tab — should return action_id in response
curl -s -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://example.com"}' | python3 -m json.tool | head -5
```

Expected: Response starts with `"action_id": "XXXXXX"` (6 uppercase alphanumeric chars)

**Step 3: Click and verify action_id**

```bash
# Get tab id
TAB_ID=$(curl -s http://localhost:8222/api/v1/tabs | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['id'])")

# Click — should return action_id
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB_ID/click" \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":100}' | python3 -c "import sys,json; d=json.load(sys.stdin); print('action_id:', d.get('action_id'))"
```

Expected: `action_id: XXXXXX`

**Step 4: Verify action_id in history**

```bash
# List recent actions — should show string IDs
curl -s "http://localhost:8222/api/v1/history/actions?limit=5" | python3 -m json.tool | grep '"id"'
```

Expected: `"id": "XXXXXX"` (string, not integer)

**Step 5: Verify error response includes action_id**

```bash
# Click on non-existent tab — should return action_id in error
curl -s -X POST "http://localhost:8222/api/v1/tabs/nonexistent/click" \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":100}' | python3 -m json.tool
```

Expected: Response includes `"action_id"` field alongside `"error"`

**Step 6: Shutdown and commit test verification**

```bash
curl -s -X POST http://localhost:8222/api/v1/browser/shutdown
```

No commit needed for manual verification — just confirm everything works.
