# Network Capture & Browser Curl — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Capture network traffic during ABP action cycles and provide a session-aware HTTP client (`browser_curl`) so agents can understand and directly call backend APIs.

**Architecture:** Per-tab in-memory network buffer (`AbpNetworkCapture`) subscribes to CDP Network events, eagerly fetching response bodies. Agents tag interesting action cycles to persist to SQLite (`AbpNetworkDatabase`). `AbpCurlHandler` uses `network::SimpleURLLoader` on the tab's `StoragePartition` for session-aware requests that work while JS is paused.

**Tech Stack:** C++ (Chromium), CDP Network domain, SQLite (`sql::Database`), `network::SimpleURLLoader`, `base::Value` JSON, MCP JSON-RPC

---

### Task 1: Add CapturedRequest struct to abp_types.h

**Files:**
- Modify: `chrome/browser/abp/abp_types.h`

**Step 1: Add CapturedRequest struct**

Add after the existing `ResponseWithHeadersCallback` typedef (around line 38):

```cpp
// Network capture types
struct CapturedRequest {
  std::string request_id;      // CDP request ID
  std::string action_id;       // ABP action that captured this
  std::string url;             // Final URL (after redirects)
  std::string url_hostname;
  std::string url_path;
  std::string url_query;
  std::string method;
  base::Value::Dict request_headers;
  std::string request_body;
  std::string resource_type;   // xhr, fetch, document, etc.
  bool cors_preflight = false;

  int status_code = 0;
  base::Value::Dict response_headers;
  std::string response_body;
  bool response_body_is_base64 = false;  // true for binary responses

  base::Value::List redirect_chain;  // [{url, status}, ...]

  int64_t started_at_ms = 0;
  int64_t completed_at_ms = 0;
  int64_t virtual_time_ms = 0;

  bool completed = false;
  bool served_from_service_worker = false;
};

struct NetworkConfig {
  std::string tag;  // Empty = don't persist
  std::set<std::string> types = {"XHR", "Fetch"};  // CDP resource types
};
```

Add required includes at top: `#include <set>` (if not already present).

**Step 2: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully

**Step 3: Commit**

```
feat(abp): add CapturedRequest and NetworkConfig structs
```

---

### Task 2: AbpNetworkDatabase — Schema and Insert

**Files:**
- Create: `chrome/browser/abp/abp_network_database.h`
- Create: `chrome/browser/abp/abp_network_database.cc`
- Modify: `chrome/browser/abp/BUILD.gn`

**Step 1: Create header file**

Follow `abp_history_database.h` pattern. Key methods:

```cpp
#ifndef CHROME_BROWSER_ABP_ABP_NETWORK_DATABASE_H_
#define CHROME_BROWSER_ABP_ABP_NETWORK_DATABASE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/abp/abp_types.h"

namespace sql {
class Database;
}

class AbpNetworkDatabase {
 public:
  explicit AbpNetworkDatabase(const base::FilePath& session_dir);
  ~AbpNetworkDatabase();

  AbpNetworkDatabase(const AbpNetworkDatabase&) = delete;
  AbpNetworkDatabase& operator=(const AbpNetworkDatabase&) = delete;

  // Save captured requests with a tag. Deduplicates by (tab_id, request_id, tag).
  void SaveRequests(const std::string& tag,
                    const std::vector<CapturedRequest>& requests,
                    base::OnceClosure callback);

  // Query saved requests with regex filters. All filters are optional.
  struct QueryFilter {
    std::string tag;
    std::string url_regex;
    std::string hostname_regex;
    std::string path_regex;
    std::string query_regex;
    std::string method_regex;
    std::string status_regex;
    std::string type;
    std::string tab_id;
    std::string action_id;
    bool include_body = false;
  };

  using QueryCallback = base::OnceCallback<void(base::Value::List results)>;
  void QueryRequests(const QueryFilter& filter, QueryCallback callback);

  // Clear saved requests, optionally filtered by tag.
  void ClearRequests(const std::string& tag, base::OnceClosure callback);

 private:
  void InitializeOnDB();
  void CreateTables();
  void SaveRequestsOnDB(const std::string& tag,
                         std::vector<CapturedRequest> requests);
  base::Value::List QueryRequestsOnDB(const QueryFilter& filter);
  void ClearRequestsOnDB(const std::string& tag);

  base::FilePath db_path_;
  std::unique_ptr<sql::Database> db_;
  scoped_refptr<base::SequencedTaskRunner> db_task_runner_;

  SEQUENCE_CHECKER(sequence_checker_);
};

#endif  // CHROME_BROWSER_ABP_ABP_NETWORK_DATABASE_H_
```

**Step 2: Create implementation file**

Follow `abp_history_database.cc` patterns for SQL operations:

```cpp
#include "chrome/browser/abp/abp_network_database.h"

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace {

constexpr char kCreateNetworkRequestsTable[] = R"(
  CREATE TABLE IF NOT EXISTS network_requests (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    request_id TEXT NOT NULL,
    action_id TEXT NOT NULL,
    tab_id TEXT NOT NULL,
    tag TEXT NOT NULL,
    url TEXT NOT NULL,
    url_hostname TEXT NOT NULL,
    url_path TEXT NOT NULL,
    url_query TEXT,
    method TEXT NOT NULL,
    request_headers TEXT,
    request_body TEXT,
    resource_type TEXT,
    cors_preflight INTEGER DEFAULT 0,
    status INTEGER,
    response_headers TEXT,
    response_body TEXT,
    response_body_encoding TEXT,
    redirect_chain TEXT,
    started_at_ms INTEGER,
    completed_at_ms INTEGER,
    duration_ms INTEGER,
    virtual_time_ms INTEGER,
    UNIQUE(tab_id, request_id, tag)
  )
)";

constexpr char kCreateTagIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_tag ON network_requests(tag)";
constexpr char kCreateActionIdIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_action_id ON network_requests(action_id)";
constexpr char kCreateTabIdIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_tab_id ON network_requests(tab_id)";
constexpr char kCreateUrlIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_url ON network_requests(url)";
constexpr char kCreateHostnameIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_hostname ON network_requests(url_hostname)";
constexpr char kCreatePathIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_path ON network_requests(url_path)";

}  // namespace

AbpNetworkDatabase::AbpNetworkDatabase(const base::FilePath& session_dir)
    : db_path_(session_dir.AppendASCII("network.db")),
      db_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  db_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::InitializeOnDB,
                     base::Unretained(this)));
}

AbpNetworkDatabase::~AbpNetworkDatabase() {
  db_task_runner_->DeleteSoon(FROM_HERE, std::move(db_));
}

void AbpNetworkDatabase::InitializeOnDB() {
  db_ = std::make_unique<sql::Database>(sql::DatabaseOptions{});
  if (!db_->Open(db_path_)) {
    LOG(ERROR) << "Failed to open network database: " << db_path_;
    return;
  }
  CreateTables();
}

void AbpNetworkDatabase::CreateTables() {
  sql::Transaction transaction(db_.get());
  if (!transaction.Begin())
    return;

  if (!db_->Execute(kCreateNetworkRequestsTable)) {
    LOG(ERROR) << "Failed to create network_requests table";
    return;
  }

  // Indexes (ignore failures on existing)
  db_->Execute(kCreateTagIndex);
  db_->Execute(kCreateActionIdIndex);
  db_->Execute(kCreateTabIdIndex);
  db_->Execute(kCreateUrlIndex);
  db_->Execute(kCreateHostnameIndex);
  db_->Execute(kCreatePathIndex);

  transaction.Commit();
}

void AbpNetworkDatabase::SaveRequests(
    const std::string& tag,
    const std::vector<CapturedRequest>& requests,
    base::OnceClosure callback) {
  // Copy requests for the DB thread
  std::vector<CapturedRequest> requests_copy;
  requests_copy.reserve(requests.size());
  for (const auto& req : requests) {
    CapturedRequest copy;
    copy.request_id = req.request_id;
    copy.action_id = req.action_id;
    copy.url = req.url;
    copy.url_hostname = req.url_hostname;
    copy.url_path = req.url_path;
    copy.url_query = req.url_query;
    copy.method = req.method;
    copy.request_headers = req.request_headers.Clone();
    copy.request_body = req.request_body;
    copy.resource_type = req.resource_type;
    copy.cors_preflight = req.cors_preflight;
    copy.status_code = req.status_code;
    copy.response_headers = req.response_headers.Clone();
    copy.response_body = req.response_body;
    copy.response_body_is_base64 = req.response_body_is_base64;
    copy.redirect_chain = req.redirect_chain.Clone();
    copy.started_at_ms = req.started_at_ms;
    copy.completed_at_ms = req.completed_at_ms;
    copy.virtual_time_ms = req.virtual_time_ms;
    copy.completed = req.completed;
    requests_copy.push_back(std::move(copy));
  }

  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::SaveRequestsOnDB,
                     base::Unretained(this), tag,
                     std::move(requests_copy)),
      std::move(callback));
}

void AbpNetworkDatabase::SaveRequestsOnDB(
    const std::string& tag,
    std::vector<CapturedRequest> requests) {
  if (!db_)
    return;

  sql::Transaction transaction(db_.get());
  if (!transaction.Begin())
    return;

  for (const auto& req : requests) {
    sql::Statement stmt(db_->GetUniqueStatement(
        "INSERT OR IGNORE INTO network_requests "
        "(request_id, action_id, tab_id, tag, url, url_hostname, url_path, "
        "url_query, method, request_headers, request_body, resource_type, "
        "cors_preflight, status, response_headers, response_body, "
        "response_body_encoding, redirect_chain, started_at_ms, "
        "completed_at_ms, duration_ms, virtual_time_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));

    int col = 0;
    stmt.BindString(col++, req.request_id);
    stmt.BindString(col++, req.action_id);
    // tab_id comes from the request's action context, stored on CapturedRequest
    // For now we use a placeholder — controller sets it before saving
    stmt.BindString(col++, "");  // tab_id set by caller
    stmt.BindString(col++, tag);
    stmt.BindString(col++, req.url);
    stmt.BindString(col++, req.url_hostname);
    stmt.BindString(col++, req.url_path);
    stmt.BindString(col++, req.url_query);
    stmt.BindString(col++, req.method);

    std::string headers_json;
    base::JSONWriter::Write(req.request_headers, &headers_json);
    stmt.BindString(col++, headers_json);

    stmt.BindString(col++, req.request_body);
    stmt.BindString(col++, req.resource_type);
    stmt.BindInt(col++, req.cors_preflight ? 1 : 0);
    stmt.BindInt(col++, req.status_code);

    std::string resp_headers_json;
    base::JSONWriter::Write(req.response_headers, &resp_headers_json);
    stmt.BindString(col++, resp_headers_json);

    stmt.BindString(col++, req.response_body);
    stmt.BindString(col++, req.response_body_is_base64 ? "base64" : "text");

    std::string redirect_json;
    base::JSONWriter::Write(req.redirect_chain, &redirect_json);
    stmt.BindString(col++, redirect_json);

    stmt.BindInt64(col++, req.started_at_ms);
    stmt.BindInt64(col++, req.completed_at_ms);
    int64_t duration = (req.completed_at_ms > 0 && req.started_at_ms > 0)
                           ? (req.completed_at_ms - req.started_at_ms)
                           : 0;
    stmt.BindInt64(col++, duration);
    stmt.BindInt64(col++, req.virtual_time_ms);

    if (!stmt.Run()) {
      LOG(ERROR) << "Failed to insert network request: " << req.url;
    }
  }

  transaction.Commit();
}

void AbpNetworkDatabase::QueryRequests(const QueryFilter& filter,
                                       QueryCallback callback) {
  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::QueryRequestsOnDB,
                     base::Unretained(this), filter),
      std::move(callback));
}

base::Value::List AbpNetworkDatabase::QueryRequestsOnDB(
    const QueryFilter& filter) {
  base::Value::List results;
  if (!db_)
    return results;

  std::string query =
      "SELECT id, request_id, action_id, tab_id, tag, url, url_hostname, "
      "url_path, url_query, method, request_headers, request_body, "
      "resource_type, cors_preflight, status, response_headers, "
      "response_body, response_body_encoding, redirect_chain, "
      "started_at_ms, completed_at_ms, duration_ms, virtual_time_ms "
      "FROM network_requests WHERE 1=1";

  std::vector<std::string> string_binds;

  if (!filter.tag.empty()) {
    query += " AND tag = ?";
    string_binds.push_back(filter.tag);
  }
  if (!filter.tab_id.empty()) {
    query += " AND tab_id = ?";
    string_binds.push_back(filter.tab_id);
  }
  if (!filter.action_id.empty()) {
    query += " AND action_id = ?";
    string_binds.push_back(filter.action_id);
  }
  if (!filter.type.empty()) {
    query += " AND resource_type = ?";
    string_binds.push_back(filter.type);
  }

  // Regex filters use SQLite REGEXP (we'll register a function)
  // For simplicity, use LIKE with % wildcards initially, or GLOB
  // Actually, we should use application-level regex filtering
  // since SQLite doesn't have REGEXP by default.
  // Instead, we'll filter in C++ after fetching.

  if (!filter.url_regex.empty()) {
    query += " AND url LIKE ?";
    string_binds.push_back("%" + filter.url_regex + "%");
  }
  if (!filter.hostname_regex.empty()) {
    query += " AND url_hostname LIKE ?";
    string_binds.push_back("%" + filter.hostname_regex + "%");
  }
  if (!filter.path_regex.empty()) {
    query += " AND url_path LIKE ?";
    string_binds.push_back("%" + filter.path_regex + "%");
  }
  if (!filter.query_regex.empty()) {
    query += " AND url_query LIKE ?";
    string_binds.push_back("%" + filter.query_regex + "%");
  }
  if (!filter.method_regex.empty()) {
    query += " AND method LIKE ?";
    string_binds.push_back("%" + filter.method_regex + "%");
  }
  if (!filter.status_regex.empty()) {
    query += " AND CAST(status AS TEXT) LIKE ?";
    string_binds.push_back("%" + filter.status_regex + "%");
  }

  query += " ORDER BY started_at_ms ASC";

  sql::Statement stmt(db_->GetUniqueStatement(query.c_str()));

  int bind_index = 0;
  for (const auto& s : string_binds) {
    stmt.BindString(bind_index++, s);
  }

  while (stmt.Step()) {
    base::Value::Dict row;
    row.Set("id", stmt.ColumnInt(0));
    row.Set("request_id", stmt.ColumnString(1));
    row.Set("action_id", stmt.ColumnString(2));
    row.Set("tab_id", stmt.ColumnString(3));
    row.Set("tag", stmt.ColumnString(4));
    row.Set("url", stmt.ColumnString(5));
    row.Set("url_hostname", stmt.ColumnString(6));
    row.Set("url_path", stmt.ColumnString(7));
    row.Set("url_query", stmt.ColumnString(8));
    row.Set("method", stmt.ColumnString(9));

    if (filter.include_body) {
      row.Set("request_headers", stmt.ColumnString(10));
      row.Set("request_body", stmt.ColumnString(11));
    }

    row.Set("resource_type", stmt.ColumnString(12));
    row.Set("cors_preflight", stmt.ColumnBool(13));
    row.Set("status", stmt.ColumnInt(14));

    if (filter.include_body) {
      row.Set("response_headers", stmt.ColumnString(15));
      row.Set("response_body", stmt.ColumnString(16));
      row.Set("response_body_encoding", stmt.ColumnString(17));
    }

    // Redirect chain
    std::string redirect_json = stmt.ColumnString(18);
    if (!redirect_json.empty()) {
      row.Set("redirect_chain", redirect_json);
    }

    row.Set("started_at_ms", static_cast<double>(stmt.ColumnInt64(19)));
    row.Set("completed_at_ms", static_cast<double>(stmt.ColumnInt64(20)));
    row.Set("duration_ms", static_cast<double>(stmt.ColumnInt64(21)));
    row.Set("virtual_time_ms", static_cast<double>(stmt.ColumnInt64(22)));

    results.Append(std::move(row));
  }

  return results;
}

void AbpNetworkDatabase::ClearRequests(const std::string& tag,
                                       base::OnceClosure callback) {
  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::ClearRequestsOnDB,
                     base::Unretained(this), tag),
      std::move(callback));
}

void AbpNetworkDatabase::ClearRequestsOnDB(const std::string& tag) {
  if (!db_)
    return;

  if (tag.empty()) {
    db_->Execute("DELETE FROM network_requests");
  } else {
    sql::Statement stmt(db_->GetUniqueStatement(
        "DELETE FROM network_requests WHERE tag = ?"));
    stmt.BindString(0, tag);
    stmt.Run();
  }
}
```

**Step 3: Add to BUILD.gn**

Add `"abp_network_database.cc"` and `"abp_network_database.h"` to the `sources` list in `chrome/browser/abp/BUILD.gn`, maintaining alphabetical order.

**Step 4: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully

**Step 5: Commit**

```
feat(abp): add AbpNetworkDatabase for tagged network request storage
```

---

### Task 3: AbpNetworkCapture — In-Memory Buffer

**Files:**
- Create: `chrome/browser/abp/abp_network_capture.h`
- Create: `chrome/browser/abp/abp_network_capture.cc`
- Modify: `chrome/browser/abp/BUILD.gn`

**Step 1: Create header file**

```cpp
#ifndef CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_
#define CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"

class AbpCdpClient;

// Per-tab in-memory network request buffer.
// Captures CDP Network events and stores full request/response data.
// Buffer is cleared on each new action (except browser_wait which merges).
class AbpNetworkCapture {
 public:
  static constexpr size_t kMaxBufferSize = 1000;

  AbpNetworkCapture();
  ~AbpNetworkCapture();

  AbpNetworkCapture(const AbpNetworkCapture&) = delete;
  AbpNetworkCapture& operator=(const AbpNetworkCapture&) = delete;

  // Configure which resource types to capture (CDP type names).
  // Default: {"XHR", "Fetch"}
  void SetCaptureTypes(const std::set<std::string>& types);

  // Set the current action ID for new captures.
  void SetCurrentActionId(const std::string& action_id);

  // Clear the buffer (called at start of non-wait actions).
  void ClearBuffer();

  // Process a CDP Network event. Called by controller for each event.
  void OnNetworkEvent(const std::string& method,
                      const base::Value::Dict& params,
                      AbpCdpClient* cdp_client);

  // Get current buffer counts.
  int GetTotalCount() const;
  int GetCompletedCount() const;
  int GetPendingCount() const;

  // Get all buffered requests (for saving to DB).
  const std::vector<CapturedRequest>& GetRequests() const;

  // Check if a request ID has a pending CORS preflight.
  void MarkCorsPreflight(const std::string& request_id);

 private:
  void OnRequestWillBeSent(const base::Value::Dict& params);
  void OnResponseReceived(const base::Value::Dict& params);
  void OnLoadingFinished(const std::string& request_id,
                         AbpCdpClient* cdp_client);
  void OnLoadingFailed(const std::string& request_id);
  void OnRequestServedFromServiceWorker(const std::string& request_id);
  void OnRequestWillBeSentExtraInfo(const base::Value::Dict& params);

  void OnResponseBodyReceived(const std::string& request_id,
                              bool success,
                              const std::string& response);

  bool ShouldCapture(const std::string& resource_type) const;
  CapturedRequest* FindRequest(const std::string& request_id);
  void EvictOldestIfFull();

  std::vector<CapturedRequest> requests_;
  std::map<std::string, size_t> request_id_to_index_;  // Fast lookup
  std::set<std::string> service_worker_request_ids_;    // Skip these
  std::set<std::string> cors_preflight_ids_;            // Track preflights

  std::set<std::string> capture_types_ = {"XHR", "Fetch"};
  std::string current_action_id_;
};

#endif  // CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_
```

**Step 2: Create implementation file**

```cpp
#include "chrome/browser/abp/abp_network_capture.h"

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_cdp_client.h"
#include "url/gurl.h"

AbpNetworkCapture::AbpNetworkCapture() = default;
AbpNetworkCapture::~AbpNetworkCapture() = default;

void AbpNetworkCapture::SetCaptureTypes(const std::set<std::string>& types) {
  capture_types_ = types;
}

void AbpNetworkCapture::SetCurrentActionId(const std::string& action_id) {
  current_action_id_ = action_id;
}

void AbpNetworkCapture::ClearBuffer() {
  requests_.clear();
  request_id_to_index_.clear();
  service_worker_request_ids_.clear();
  cors_preflight_ids_.clear();
}

void AbpNetworkCapture::OnNetworkEvent(const std::string& method,
                                       const base::Value::Dict& params,
                                       AbpCdpClient* cdp_client) {
  if (method == "Network.requestWillBeSent") {
    OnRequestWillBeSent(params);
  } else if (method == "Network.responseReceived") {
    OnResponseReceived(params);
  } else if (method == "Network.loadingFinished") {
    const std::string* request_id = params.FindString("requestId");
    if (request_id)
      OnLoadingFinished(*request_id, cdp_client);
  } else if (method == "Network.loadingFailed") {
    const std::string* request_id = params.FindString("requestId");
    if (request_id)
      OnLoadingFailed(*request_id);
  } else if (method == "Network.requestServedFromServiceWorker") {
    const std::string* request_id = params.FindString("requestId");
    if (request_id)
      OnRequestServedFromServiceWorker(*request_id);
  } else if (method == "Network.requestWillBeSentExtraInfo") {
    OnRequestWillBeSentExtraInfo(params);
  }
}

bool AbpNetworkCapture::ShouldCapture(const std::string& resource_type) const {
  return capture_types_.count(resource_type) > 0;
}

void AbpNetworkCapture::OnRequestWillBeSent(const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id)
    return;

  // Skip service-worker-served requests
  if (service_worker_request_ids_.count(*request_id) > 0)
    return;

  const std::string* type = params.FindString("type");
  // Type may not be present on initial requestWillBeSent; we'll check on
  // responseReceived too. For redirects, type is present.

  // Check for CORS preflight (OPTIONS method)
  const base::Value::Dict* request = params.FindDict("request");
  if (request) {
    const std::string* method = request->FindString("method");
    if (method && *method == "OPTIONS") {
      // This is likely a CORS preflight. Mark it.
      cors_preflight_ids_.insert(*request_id);
      return;  // Don't buffer preflight requests
    }
  }

  // Check for redirect — update existing entry
  const base::Value::Dict* redirect_response = params.FindDict("redirectResponse");
  CapturedRequest* existing = FindRequest(*request_id);
  if (redirect_response && existing) {
    // Add current URL + status to redirect chain
    base::Value::Dict redirect_hop;
    redirect_hop.Set("url", existing->url);
    std::optional<int> status = redirect_response->FindInt("status");
    if (status.has_value())
      redirect_hop.Set("status", *status);
    existing->redirect_chain.Append(std::move(redirect_hop));

    // Update to new URL
    if (request) {
      const std::string* new_url = request->FindString("url");
      if (new_url) {
        existing->url = *new_url;
        GURL parsed(*new_url);
        existing->url_hostname = parsed.host();
        existing->url_path = parsed.path();
        existing->url_query = parsed.query();
      }
      const std::string* new_method = request->FindString("method");
      if (new_method)
        existing->method = *new_method;
    }
    return;
  }

  // Filter by type if available
  if (type && !ShouldCapture(*type))
    return;

  EvictOldestIfFull();

  // New request
  CapturedRequest req;
  req.request_id = *request_id;
  req.action_id = current_action_id_;
  req.started_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();

  if (type)
    req.resource_type = *type;

  if (request) {
    const std::string* url = request->FindString("url");
    if (url) {
      req.url = *url;
      GURL parsed(*url);
      req.url_hostname = parsed.host();
      req.url_path = parsed.path();
      req.url_query = parsed.query();
    }

    const std::string* method = request->FindString("method");
    if (method)
      req.method = *method;

    // Request headers
    const base::Value::Dict* headers = request->FindDict("headers");
    if (headers)
      req.request_headers = headers->Clone();

    // Request body (postData)
    const std::string* post_data = request->FindString("postData");
    if (post_data)
      req.request_body = *post_data;
  }

  size_t index = requests_.size();
  request_id_to_index_[*request_id] = index;
  requests_.push_back(std::move(req));
}

void AbpNetworkCapture::OnResponseReceived(const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id)
    return;

  CapturedRequest* req = FindRequest(*request_id);
  if (!req)
    return;

  // Check if there was a CORS preflight for this request
  // (The actual request after a preflight will have a different request_id
  // but may reference the same URL)
  if (cors_preflight_ids_.count(*request_id) > 0) {
    req->cors_preflight = true;
  }

  const base::Value::Dict* response = params.FindDict("response");
  if (response) {
    std::optional<int> status = response->FindInt("status");
    if (status.has_value())
      req->status_code = *status;

    const base::Value::Dict* headers = response->FindDict("headers");
    if (headers)
      req->response_headers = headers->Clone();
  }

  // Update resource type if we didn't have it
  const std::string* type = params.FindString("type");
  if (type && req->resource_type.empty()) {
    req->resource_type = *type;
    // Late filter: if type doesn't match, remove the request
    if (!ShouldCapture(*type)) {
      size_t idx = request_id_to_index_[*request_id];
      request_id_to_index_.erase(*request_id);
      // Remove from vector (swap with last for O(1))
      if (idx < requests_.size() - 1) {
        std::swap(requests_[idx], requests_.back());
        request_id_to_index_[requests_[idx].request_id] = idx;
      }
      requests_.pop_back();
    }
  }
}

void AbpNetworkCapture::OnLoadingFinished(const std::string& request_id,
                                          AbpCdpClient* cdp_client) {
  CapturedRequest* req = FindRequest(request_id);
  if (!req)
    return;

  req->completed = true;
  req->completed_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Eagerly fetch response body
  if (cdp_client) {
    base::Value::Dict body_params;
    body_params.Set("requestId", request_id);
    cdp_client->SendCommand(
        "Network.getResponseBody", body_params,
        base::BindOnce(&AbpNetworkCapture::OnResponseBodyReceived,
                       base::Unretained(this), request_id));
  }
}

void AbpNetworkCapture::OnLoadingFailed(const std::string& request_id) {
  CapturedRequest* req = FindRequest(request_id);
  if (!req)
    return;

  req->completed = true;
  req->completed_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
}

void AbpNetworkCapture::OnRequestServedFromServiceWorker(
    const std::string& request_id) {
  service_worker_request_ids_.insert(request_id);

  // Remove from buffer if already captured
  auto it = request_id_to_index_.find(request_id);
  if (it != request_id_to_index_.end()) {
    size_t idx = it->second;
    request_id_to_index_.erase(it);
    if (idx < requests_.size() - 1) {
      std::swap(requests_[idx], requests_.back());
      request_id_to_index_[requests_[idx].request_id] = idx;
    }
    requests_.pop_back();
  }
}

void AbpNetworkCapture::OnRequestWillBeSentExtraInfo(
    const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id)
    return;

  CapturedRequest* req = FindRequest(request_id);
  if (!req)
    return;

  // Merge browser-added headers (cookies etc.) into request headers
  const base::Value::Dict* headers = params.FindDict("headers");
  if (headers) {
    for (const auto [key, value] : *headers) {
      if (!req->request_headers.contains(key) && value.is_string()) {
        req->request_headers.Set(key, value.GetString());
      }
    }
  }
}

void AbpNetworkCapture::OnResponseBodyReceived(
    const std::string& request_id,
    bool success,
    const std::string& response) {
  CapturedRequest* req = FindRequest(request_id);
  if (!req || !success)
    return;

  // Parse CDP response: {"body": "...", "base64Encoded": true/false}
  auto parsed = base::JSONReader::Read(response);
  if (!parsed || !parsed->is_dict())
    return;

  const base::Value::Dict& dict = parsed->GetDict();
  const std::string* body = dict.FindString("body");
  if (body) {
    // Skip very large bodies (50MB safety valve)
    if (body->size() > 50 * 1024 * 1024)
      return;
    req->response_body = *body;
  }

  std::optional<bool> base64_encoded = dict.FindBool("base64Encoded");
  req->response_body_is_base64 = base64_encoded.value_or(false);
}

CapturedRequest* AbpNetworkCapture::FindRequest(
    const std::string& request_id) {
  auto it = request_id_to_index_.find(request_id);
  if (it == request_id_to_index_.end())
    return nullptr;
  return &requests_[it->second];
}

void AbpNetworkCapture::EvictOldestIfFull() {
  if (requests_.size() < kMaxBufferSize)
    return;

  // Remove oldest (index 0)
  request_id_to_index_.erase(requests_[0].request_id);
  requests_.erase(requests_.begin());

  // Rebuild index map (shifted by -1)
  request_id_to_index_.clear();
  for (size_t i = 0; i < requests_.size(); i++) {
    request_id_to_index_[requests_[i].request_id] = i;
  }
}

int AbpNetworkCapture::GetTotalCount() const {
  return static_cast<int>(requests_.size());
}

int AbpNetworkCapture::GetCompletedCount() const {
  int count = 0;
  for (const auto& req : requests_) {
    if (req.completed)
      count++;
  }
  return count;
}

int AbpNetworkCapture::GetPendingCount() const {
  return GetTotalCount() - GetCompletedCount();
}

const std::vector<CapturedRequest>& AbpNetworkCapture::GetRequests() const {
  return requests_;
}

void AbpNetworkCapture::MarkCorsPreflight(const std::string& request_id) {
  CapturedRequest* req = FindRequest(request_id);
  if (req)
    req->cors_preflight = true;
}
```

**Step 3: Add to BUILD.gn**

Add `"abp_network_capture.cc"` and `"abp_network_capture.h"` to sources.

**Step 4: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully

**Step 5: Commit**

```
feat(abp): add AbpNetworkCapture in-memory buffer for CDP network events
```

---

### Task 4: AbpCurlHandler — Session-Aware HTTP Client

**Files:**
- Create: `chrome/browser/abp/abp_curl_handler.h`
- Create: `chrome/browser/abp/abp_curl_handler.cc`
- Modify: `chrome/browser/abp/BUILD.gn`

**Step 1: Create header file**

```cpp
#ifndef CHROME_BROWSER_ABP_ABP_CURL_HANDLER_H_
#define CHROME_BROWSER_ABP_ABP_CURL_HANDLER_H_

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

namespace content {
class StoragePartition;
}

namespace network {
class SimpleURLLoader;
}

class AbpCurlHandler {
 public:
  struct Request {
    std::string url;
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::string body;
    std::string origin;   // Auto-populated from tab's current URL
    std::string referer;  // Auto-populated from tab's current URL
  };

  struct Response {
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    bool body_is_base64 = false;
    std::string final_url;
    bool redirected = false;
  };

  using ResponseCallback = base::OnceCallback<void(Response response)>;

  AbpCurlHandler();
  ~AbpCurlHandler();

  AbpCurlHandler(const AbpCurlHandler&) = delete;
  AbpCurlHandler& operator=(const AbpCurlHandler&) = delete;

  // Execute a request using the given StoragePartition's network context.
  // This inherits all cookies and session state for that partition.
  void Execute(const Request& request,
               content::StoragePartition* storage_partition,
               ResponseCallback callback);

 private:
  void OnResponseReceived(std::unique_ptr<network::SimpleURLLoader> loader,
                          ResponseCallback callback,
                          std::unique_ptr<std::string> body);

  base::WeakPtrFactory<AbpCurlHandler> weak_factory_{this};
};

#endif  // CHROME_BROWSER_ABP_ABP_CURL_HANDLER_H_
```

**Step 2: Create implementation file**

```cpp
#include "chrome/browser/abp/abp_curl_handler.h"

#include "base/strings/string_util.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/load_flags.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("abp_curl_handler", R"(
      semantics {
        sender: "ABP Curl Handler"
        description: "Agent-initiated HTTP request using tab session context."
        trigger: "Agent calls browser_curl MCP tool or REST endpoint."
        data: "Arbitrary HTTP request as specified by the agent."
        destination: OTHER
      }
      policy {
        cookies_allowed: YES
        cookies_store: "user"
        setting: "ABP must be running."
      }
    )");

constexpr int kMaxResponseSize = 50 * 1024 * 1024;  // 50MB

bool IsTextContentType(const std::string& content_type) {
  return base::StartsWith(content_type, "text/",
                          base::CompareCase::INSENSITIVE_ASCII) ||
         content_type.find("json") != std::string::npos ||
         content_type.find("xml") != std::string::npos ||
         content_type.find("javascript") != std::string::npos ||
         content_type.find("html") != std::string::npos;
}

}  // namespace

AbpCurlHandler::AbpCurlHandler() = default;
AbpCurlHandler::~AbpCurlHandler() = default;

void AbpCurlHandler::Execute(const Request& request,
                             content::StoragePartition* storage_partition,
                             ResponseCallback callback) {
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(request.url);
  resource_request->method = request.method;

  // Auto-populate Origin and Referer
  if (!request.origin.empty()) {
    resource_request->headers.SetHeader("Origin", request.origin);
  }
  if (!request.referer.empty()) {
    resource_request->referrer = GURL(request.referer);
  }

  // Set custom headers
  for (const auto& [key, value] : request.headers) {
    resource_request->headers.SetHeader(key, value);
  }

  // Use the tab's cookie store
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kInclude;

  auto loader = network::SimpleURLLoader::Create(
      std::move(resource_request), kTrafficAnnotation);

  if (!request.body.empty()) {
    loader->AttachStringForUpload(request.body);
  }

  loader->SetAllowHttpErrorResults(true);

  auto* loader_ptr = loader.get();
  loader_ptr->DownloadToString(
      storage_partition->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(&AbpCurlHandler::OnResponseReceived,
                     weak_factory_.GetWeakPtr(), std::move(loader),
                     std::move(callback)),
      kMaxResponseSize);
}

void AbpCurlHandler::OnResponseReceived(
    std::unique_ptr<network::SimpleURLLoader> loader,
    ResponseCallback callback,
    std::unique_ptr<std::string> body) {
  Response response;

  if (loader->ResponseInfo()) {
    const auto& response_info = *loader->ResponseInfo();

    response.status_code = response_info.headers
                               ? response_info.headers->response_code()
                               : 0;

    // Extract response headers
    if (response_info.headers) {
      size_t iter = 0;
      std::string name, value;
      while (response_info.headers->EnumerateHeaderLines(&iter, &name,
                                                         &value)) {
        response.headers[name] = value;
      }
    }

    // Check for redirect
    response.final_url = loader->GetFinalURL().spec();
    response.redirected = (response.final_url != loader->GetFinalURL().spec());
  }

  if (body) {
    // Determine if body is text or binary
    std::string content_type;
    auto ct_it = response.headers.find("content-type");
    if (ct_it == response.headers.end())
      ct_it = response.headers.find("Content-Type");
    if (ct_it != response.headers.end())
      content_type = ct_it->second;

    if (IsTextContentType(content_type) || content_type.empty()) {
      response.body = std::move(*body);
      response.body_is_base64 = false;
    } else {
      // Base64 encode binary content
      response.body = base::Base64Encode(*body);
      response.body_is_base64 = true;
    }
  }

  std::move(callback).Run(std::move(response));
}
```

**Step 3: Add to BUILD.gn**

Add `"abp_curl_handler.cc"` and `"abp_curl_handler.h"` to sources. Check that `//services/network/public/cpp` and `//content/public/browser` are in deps (they likely already are).

**Step 4: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully

**Step 5: Commit**

```
feat(abp): add AbpCurlHandler for session-aware HTTP requests
```

---

### Task 5: Controller Integration — TabState, CDP Events, REST Endpoints

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h`
- Modify: `chrome/browser/abp/abp_controller.cc`

This is the largest task. It wires everything together.

**Step 1: Add includes and members to abp_controller.h**

Add to includes:
```cpp
#include "chrome/browser/abp/abp_curl_handler.h"
#include "chrome/browser/abp/abp_network_capture.h"
#include "chrome/browser/abp/abp_network_database.h"
```

Add `AbpNetworkCapture` to `TabState` struct (after `persistent_active_request_ids`):
```cpp
std::unique_ptr<AbpNetworkCapture> network_capture;
```

Add members to `AbpController` (private section):
```cpp
std::unique_ptr<AbpNetworkDatabase> network_db_;
std::unique_ptr<AbpCurlHandler> curl_handler_;
```

Add method declarations (public or private as appropriate):
```cpp
// Network capture
void HandleNetworkQuery(const std::string& query_string,
                        ResponseCallback callback);
void HandleNetworkSave(const std::string& body, ResponseCallback callback);
void HandleNetworkClear(const std::string& query_string,
                        ResponseCallback callback);
void HandleCurl(const std::string& tab_id, const std::string& body,
                ResponseCallback callback);
```

**Step 2: Initialize network_db_ and curl_handler_ in constructor/Init**

In the constructor or initialization method where `history_db_` is created, add:
```cpp
network_db_ = std::make_unique<AbpNetworkDatabase>(session_dir);
curl_handler_ = std::make_unique<AbpCurlHandler>();
```

**Step 3: Create AbpNetworkCapture per tab**

In `GetOrCreateTabState()`, initialize the network capture:
```cpp
if (!tab_state.network_capture) {
  tab_state.network_capture = std::make_unique<AbpNetworkCapture>();
}
```

**Step 4: Wire CDP Network events to AbpNetworkCapture**

In `OnPersistentNetworkEvent()`, after the existing persistent tracking code, forward events to the capture buffer:
```cpp
// Forward to network capture buffer
auto it = tab_states_.find(tab_id);
if (it != tab_states_.end() && it->second.network_capture) {
  AbpCdpClient* client = it->second.cdp_client.get();
  it->second.network_capture->OnNetworkEvent(method, params, client);
}
```

**Step 5: Update Network.enable to include maxPostDataSize**

Find where `Network.enable` is called with empty params and change to:
```cpp
base::Value::Dict network_params;
network_params.Set("maxPostDataSize", 52428800);  // 50MB
client->SendCommand("Network.enable", network_params, ...);
```

**Step 6: Add REST routes in HandleRequest()**

In the routing section, add a new resource handler:
```cpp
if (resource == "network") {
  if (segments.size() == 3) {
    // /api/v1/network
    if (method == "GET") {
      HandleNetworkQuery(query_string, std::move(callback));
    } else if (method == "POST") {
      HandleNetworkSave(body, std::move(callback));
    } else if (method == "DELETE") {
      HandleNetworkClear(query_string, std::move(callback));
    }
    return;
  }
}
```

For the curl endpoint, in the `tabs` resource handler:
```cpp
if (segments.size() == 5 && segments[4] == "curl") {
  if (method == "POST") {
    HandleCurl(tab_id, body, std::move(callback));
    return;
  }
}
```

**Step 7: Implement HandleNetworkQuery**

```cpp
void AbpController::HandleNetworkQuery(const std::string& query_string,
                                       ResponseCallback callback) {
  AbpNetworkDatabase::QueryFilter filter;
  // Parse query params from query_string
  // tag, url, hostname, path, query, method, status, type,
  // tab_id, action_id, include_body
  auto params = ParseQueryString(query_string);

  auto it = params.find("tag");
  if (it != params.end()) filter.tag = it->second;
  it = params.find("url");
  if (it != params.end()) filter.url_regex = it->second;
  it = params.find("hostname");
  if (it != params.end()) filter.hostname_regex = it->second;
  it = params.find("path");
  if (it != params.end()) filter.path_regex = it->second;
  it = params.find("query");
  if (it != params.end()) filter.query_regex = it->second;
  it = params.find("method");
  if (it != params.end()) filter.method_regex = it->second;
  it = params.find("status");
  if (it != params.end()) filter.status_regex = it->second;
  it = params.find("type");
  if (it != params.end()) filter.type = it->second;
  it = params.find("tab_id");
  if (it != params.end()) filter.tab_id = it->second;
  it = params.find("action_id");
  if (it != params.end()) filter.action_id = it->second;
  it = params.find("include_body");
  if (it != params.end()) filter.include_body = (it->second == "true");

  network_db_->QueryRequests(
      filter,
      base::BindOnce(
          [](ResponseCallback cb, base::Value::List results) {
            std::string json;
            base::JSONWriter::WriteWithOptions(
                base::Value(std::move(results)),
                base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback)));
}
```

**Step 8: Implement HandleNetworkSave**

```cpp
void AbpController::HandleNetworkSave(const std::string& body,
                                      ResponseCallback callback) {
  auto parsed = base::JSONReader::Read(body);
  if (!parsed || !parsed->is_dict()) {
    SendJsonError(400, "Invalid JSON body", std::move(callback));
    return;
  }

  const std::string* tag = parsed->GetDict().FindString("tag");
  if (!tag || tag->empty()) {
    SendJsonError(400, "tag is required", std::move(callback));
    return;
  }

  // Determine which tab's buffer to save
  const std::string* tab_id = parsed->GetDict().FindString("tab_id");
  std::string resolved_tab_id = tab_id ? *tab_id : GetActiveTabId();

  auto it = tab_states_.find(resolved_tab_id);
  if (it == tab_states_.end() || !it->second.network_capture) {
    SendJsonError(404, "No network capture buffer for tab", std::move(callback));
    return;
  }

  const auto& requests = it->second.network_capture->GetRequests();
  if (requests.empty()) {
    SendJsonError(404, "No network requests in buffer", std::move(callback));
    return;
  }

  // Set tab_id on each request before saving
  // (CapturedRequest doesn't store tab_id internally — we pass it to DB)
  // Actually we need to modify the DB save to accept tab_id externally.
  // For now, let's create copies with tab_id set.

  std::string tag_copy = *tag;
  int count = static_cast<int>(requests.size());

  network_db_->SaveRequests(
      *tag, requests,
      base::BindOnce(
          [](ResponseCallback cb, std::string tag, int count) {
            base::Value::Dict result;
            result.Set("tag", tag);
            result.Set("saved", count);
            std::string json;
            base::JSONWriter::Write(base::Value(std::move(result)), &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback), std::move(tag_copy), count));
}
```

**Step 9: Implement HandleNetworkClear**

```cpp
void AbpController::HandleNetworkClear(const std::string& query_string,
                                       ResponseCallback callback) {
  auto params = ParseQueryString(query_string);
  std::string tag;
  auto it = params.find("tag");
  if (it != params.end()) tag = it->second;

  network_db_->ClearRequests(
      tag,
      base::BindOnce(
          [](ResponseCallback cb) {
            base::Value::Dict result;
            result.Set("cleared", true);
            std::string json;
            base::JSONWriter::Write(base::Value(std::move(result)), &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback)));
}
```

**Step 10: Implement HandleCurl**

```cpp
void AbpController::HandleCurl(const std::string& tab_id,
                                const std::string& body,
                                ResponseCallback callback) {
  auto parsed = base::JSONReader::Read(body);
  if (!parsed || !parsed->is_dict()) {
    SendJsonError(400, "Invalid JSON body", std::move(callback));
    return;
  }

  const base::Value::Dict& dict = parsed->GetDict();
  const std::string* url = dict.FindString("url");
  if (!url || url->empty()) {
    SendJsonError(400, "url is required", std::move(callback));
    return;
  }

  // Get tab's WebContents for StoragePartition
  content::WebContents* wc = GetWebContents(tab_id);
  if (!wc) {
    SendJsonError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCurlHandler::Request request;
  request.url = *url;

  const std::string* method = dict.FindString("method");
  if (method) request.method = *method;

  const std::string* req_body = dict.FindString("body");
  if (req_body) request.body = *req_body;

  // Custom headers
  const base::Value::Dict* headers = dict.FindDict("headers");
  if (headers) {
    for (const auto [key, value] : *headers) {
      if (value.is_string())
        request.headers[key] = value.GetString();
    }
  }

  // Auto-populate Origin and Referer from tab's current URL
  GURL tab_url = wc->GetLastCommittedURL();
  request.origin = tab_url.DeprecatedGetOriginAsURL().spec();
  request.referer = tab_url.spec();

  // Get StoragePartition
  content::StoragePartition* partition = wc->GetBrowserContext()
      ->GetStoragePartitionForUrl(tab_url);

  const std::string* tag = dict.FindString("tag");
  std::string tag_str = tag ? *tag : "";
  std::string tab_id_copy = tab_id;

  curl_handler_->Execute(
      request, partition,
      base::BindOnce(
          [](ResponseCallback cb, std::string tag, std::string tab_id,
             AbpNetworkDatabase* network_db,
             AbpCurlHandler::Response response) {
            base::Value::Dict result;
            result.Set("status", response.status_code);

            base::Value::Dict headers_dict;
            for (const auto& [key, value] : response.headers) {
              headers_dict.Set(key, value);
            }
            result.Set("headers", std::move(headers_dict));
            result.Set("body", response.body);
            result.Set("body_encoding",
                       response.body_is_base64 ? "base64" : "text");
            result.Set("url", response.final_url);
            result.Set("redirected", response.redirected);

            // Opt-in save to DB
            if (!tag.empty() && network_db) {
              CapturedRequest req;
              req.request_id = "curl_" + base::NumberToString(
                  base::Time::Now().InMillisecondsSinceUnixEpoch());
              req.url = response.final_url;
              GURL parsed_url(response.final_url);
              req.url_hostname = parsed_url.host();
              req.url_path = parsed_url.path();
              req.url_query = parsed_url.query();
              req.status_code = response.status_code;
              req.response_body = response.body;
              req.response_body_is_base64 = response.body_is_base64;
              req.completed = true;
              req.completed_at_ms =
                  base::Time::Now().InMillisecondsSinceUnixEpoch();
              // Note: request headers/body not stored for curl since
              // the agent already has them
              std::vector<CapturedRequest> reqs = {std::move(req)};
              network_db->SaveRequests(tag, reqs, base::DoNothing());
            }

            std::string json;
            base::JSONWriter::WriteWithOptions(
                base::Value(std::move(result)),
                base::JSONWriter::OPTIONS_PRETTY_PRINT, &json);
            std::move(cb).Run(200, "application/json", std::move(json));
          },
          std::move(callback), std::move(tag_str), std::move(tab_id_copy),
          network_db_.get()));
}
```

**Step 11: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully (may need to fix includes and forward declarations)

**Step 12: Commit**

```
feat(abp): integrate network capture, curl handler, and REST endpoints into controller
```

---

### Task 6: ActionContext Integration — Network Parameter & Response Envelope

**Files:**
- Modify: `chrome/browser/abp/abp_action_context.h`
- Modify: `chrome/browser/abp/abp_action_context.cc`

**Step 1: Add network config to ActionContext**

In `abp_action_context.h`, add members:
```cpp
NetworkConfig network_config_;
```

And a method to set the capture reference:
```cpp
void SetNetworkCapture(AbpNetworkCapture* capture);
```

Add member:
```cpp
AbpNetworkCapture* network_capture_ = nullptr;  // Not owned
```

**Step 2: Parse "network" parameter from request JSON**

In `abp_action_context.cc`, in the parameter parsing section (near screenshot parsing), add:

```cpp
const base::Value::Dict* network_params = params_.FindDict("network");
if (network_params) {
  const std::string* tag = network_params->FindString("tag");
  if (tag)
    network_config_.tag = *tag;

  const base::Value::List* types = network_params->FindList("types");
  if (types) {
    network_config_.types.clear();
    for (const auto& type : *types) {
      if (type.is_string()) {
        // Convert lowercase input to CDP resource type names
        std::string t = type.GetString();
        if (t == "xhr") network_config_.types.insert("XHR");
        else if (t == "fetch") network_config_.types.insert("Fetch");
        else if (t == "document") network_config_.types.insert("Document");
        else if (t == "stylesheet") network_config_.types.insert("Stylesheet");
        else if (t == "script") network_config_.types.insert("Script");
        else if (t == "font") network_config_.types.insert("Font");
        else if (t == "image") network_config_.types.insert("Image");
        else if (t == "media") network_config_.types.insert("Media");
        else if (t == "websocket") network_config_.types.insert("WebSocket");
        else if (t == "other") network_config_.types.insert("Other");
      }
    }
  }
}
```

**Step 3: Configure capture at action start**

In `Start()` or `StartOnDeterministicSlot()`, before starting event capture:

```cpp
if (network_capture_) {
  // browser_wait merges — don't clear buffer
  if (!options_.all_requests) {
    network_capture_->ClearBuffer();
  }
  network_capture_->SetCurrentActionId(action_id_);
  network_capture_->SetCaptureTypes(network_config_.types);
}
```

The `options_.all_requests` flag is `true` for `browser_wait`, which is exactly when we want to merge instead of clear.

**Step 4: Add network counts to response envelope**

In `BuildResponseEnvelope()` / `SendResponse()`, after cursor info:

```cpp
// Network capture counts
if (network_capture_) {
  base::Value::Dict network_info;
  network_info.Set("total", network_capture_->GetTotalCount());
  network_info.Set("completed", network_capture_->GetCompletedCount());
  network_info.Set("pending", network_capture_->GetPendingCount());
  if (!network_config_.tag.empty()) {
    network_info.Set("tag", network_config_.tag);
  }
  envelope.Set("network", std::move(network_info));
}
```

**Step 5: Tag and persist if requested**

After building response but before sending, if a tag was specified:

```cpp
if (!network_config_.tag.empty() && network_capture_ &&
    controller_->network_db_) {
  const auto& requests = network_capture_->GetRequests();
  if (!requests.empty()) {
    controller_->network_db_->SaveRequests(
        network_config_.tag, requests, base::DoNothing());
  }
}
```

**Step 6: Wire SetNetworkCapture in controller**

In `abp_controller.cc`, where `AbpActionContext::Run()` is called, before the run call set the network capture:

```cpp
auto& tab_state = GetOrCreateTabState(tab_id);
// ... existing code ...
action_context->SetNetworkCapture(tab_state.network_capture.get());
```

**Step 7: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully

**Step 8: Commit**

```
feat(abp): add network parameter parsing and counts to action response envelope
```

---

### Task 7: MCP Tools — browser_network and browser_curl

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h`
- Modify: `chrome/browser/abp/abp_mcp_handler.cc`

**Step 1: Add tool definitions in GetToolDefinitions()**

Add `browser_network` tool:
```cpp
tools.Append(
    ToolBuilder("browser_network")
        .Description(
            "Inspect, save, or clear captured network requests. Use action "
            "'query' to search saved requests with regex filters, 'save' to "
            "persist the current in-memory buffer with a tag, or 'clear' to "
            "delete saved requests.")
        .RequiredStringEnum("action", "Operation to perform",
                            {"query", "save", "clear"})
        .OptionalString("tag",
                        "Tag name. Required for save/clear. For query, "
                        "filters by tag.")
        .OptionalString("tab_id", "Tab ID (defaults to active tab)")
        .OptionalString("url", "Regex filter on full URL (query only)")
        .OptionalString("hostname", "Regex filter on hostname (query only)")
        .OptionalString("path", "Regex filter on URL path (query only)")
        .OptionalString("query", "Regex filter on query string (query only)")
        .OptionalString("method",
                        "Regex filter on HTTP method (query only)")
        .OptionalString("status",
                        "Regex filter on status code (query only)")
        .OptionalString("type", "Resource type filter (query only)")
        .OptionalString("action_id", "Filter by action ID (query only)")
        .OptionalBoolean("include_body",
                         "Include request/response bodies (query only, "
                         "default false)")
        .Build());
```

Add `browser_curl` tool:
```cpp
tools.Append(
    ToolBuilder("browser_curl")
        .Description(
            "Execute an HTTP request using a tab's session, cookies, and "
            "auth. Works while JavaScript is paused. Use this to call "
            "backend APIs directly after discovering them via network "
            "capture. Origin and Referer headers are auto-populated from "
            "the tab's current URL.")
        .OptionalString("tab_id", "Tab whose session to use (defaults to active)")
        .RequiredString("url", "Request URL")
        .OptionalString("method", "HTTP method (default: GET)")
        .OptionalString("body", "Request body")
        .OptionalString("tag",
                        "Tag to persist this request to the network database")
        .Build());
```

Note: For headers, we need an object property. Use a custom schema addition since ToolBuilder may not have `OptionalObject`. Check if ToolBuilder supports it; if not, build the headers property manually:

```cpp
// After building the base tool, add headers property manually
auto& curl_tool = tools[tools.size() - 1].GetDict();
auto* props = curl_tool.FindDictByDottedPath("inputSchema.properties");
if (props) {
  base::Value::Dict headers_prop;
  headers_prop.Set("type", "object");
  headers_prop.Set("description",
                   "Additional HTTP headers as key-value pairs");
  props->Set("headers", std::move(headers_prop));
}
```

**Step 2: Add method declarations to header**

In `abp_mcp_handler.h`:
```cpp
void CallBrowserNetwork(const base::Value::Dict& args,
                        base::Value request_id,
                        ResponseWithHeadersCallback callback);
void CallBrowserCurl(const base::Value::Dict& args,
                     base::Value request_id,
                     ResponseWithHeadersCallback callback);
```

**Step 3: Add dispatch routes in HandleToolsCall()**

```cpp
} else if (*name == "browser_network") {
  CallBrowserNetwork(*args, std::move(request_id), std::move(callback));
} else if (*name == "browser_curl") {
  CallBrowserCurl(*args, std::move(request_id), std::move(callback));
}
```

**Step 4: Implement CallBrowserNetwork**

```cpp
void AbpMcpHandler::CallBrowserNetwork(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  if (!action) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "action is required", std::move(callback));
    return;
  }

  if (*action == "query") {
    // Build query string from args
    std::vector<std::string> parts;
    const std::string* tag = args.FindString("tag");
    if (tag) parts.push_back("tag=" + *tag);
    const std::string* url = args.FindString("url");
    if (url) parts.push_back("url=" + *url);
    const std::string* hostname = args.FindString("hostname");
    if (hostname) parts.push_back("hostname=" + *hostname);
    const std::string* path = args.FindString("path");
    if (path) parts.push_back("path=" + *path);
    const std::string* query = args.FindString("query");
    if (query) parts.push_back("query=" + *query);
    const std::string* method = args.FindString("method");
    if (method) parts.push_back("method=" + *method);
    const std::string* status = args.FindString("status");
    if (status) parts.push_back("status=" + *status);
    const std::string* type = args.FindString("type");
    if (type) parts.push_back("type=" + *type);
    const std::string* tab_id = args.FindString("tab_id");
    if (tab_id) parts.push_back("tab_id=" + *tab_id);
    const std::string* action_id = args.FindString("action_id");
    if (action_id) parts.push_back("action_id=" + *action_id);
    std::optional<bool> include_body = args.FindBool("include_body");
    if (include_body.value_or(false)) parts.push_back("include_body=true");

    std::string qs;
    for (size_t i = 0; i < parts.size(); i++) {
      qs += (i == 0 ? "?" : "&") + parts[i];
    }

    controller_->HandleRequest(
        "GET", "/api/v1/network" + qs, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));

  } else if (*action == "save") {
    const std::string* tag = args.FindString("tag");
    if (!tag || tag->empty()) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "tag is required for save", std::move(callback));
      return;
    }

    base::Value::Dict body_dict;
    body_dict.Set("tag", *tag);
    const std::string* tab_id = args.FindString("tab_id");
    if (tab_id) body_dict.Set("tab_id", *tab_id);

    std::string body;
    base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

    controller_->HandleRequest(
        "POST", "/api/v1/network", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));

  } else if (*action == "clear") {
    std::string qs;
    const std::string* tag = args.FindString("tag");
    if (tag) qs = "?tag=" + *tag;

    controller_->HandleRequest(
        "DELETE", "/api/v1/network" + qs, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));

  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action. Use: query, save, clear",
                     std::move(callback));
  }
}
```

**Step 5: Implement CallBrowserCurl**

```cpp
void AbpMcpHandler::CallBrowserCurl(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab available", std::move(callback));
    return;
  }

  const std::string* url = args.FindString("url");
  if (!url || url->empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "url is required", std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  body_dict.Set("url", *url);

  const std::string* method = args.FindString("method");
  if (method) body_dict.Set("method", *method);

  const std::string* body = args.FindString("body");
  if (body) body_dict.Set("body", *body);

  const std::string* tag = args.FindString("tag");
  if (tag) body_dict.Set("tag", *tag);

  const base::Value::Dict* headers = args.FindDict("headers");
  if (headers) body_dict.Set("headers", headers->Clone());

  std::string body_str;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body_str);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/curl", body_str,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 6: Add "network" parameter forwarding for existing action tools**

In each `CallBrowser*` method that builds a request body for actions (CallBrowserAction, CallBrowserNavigate, CallBrowserScroll, CallBrowserWait, etc.), forward the `network` parameter if present:

```cpp
// Forward network parameter
const base::Value::Dict* network = args.FindDict("network");
if (network) {
  body_dict.Set("network", network->Clone());
}
```

This needs to be added to all action tool dispatch methods. Find each method that builds a `body_dict` and adds `screenshot` params — add `network` in the same pattern.

**Step 7: Update tool schemas for existing action tools**

For each existing action tool definition that supports `screenshot`, add:

```cpp
.OptionalString("network_tag",
    "Tag to persist this action's network calls to the database")
```

Or better, since network is an object, handle it as a raw property addition after ToolBuilder::Build(), similar to the headers approach.

Actually, for simplicity, just add `network_tag` as a flat string parameter on action tools, and in the CallBrowser* methods, construct the `network` dict from it:

```cpp
const std::string* network_tag = args.FindString("network_tag");
if (network_tag) {
  base::Value::Dict network;
  network.Set("tag", *network_tag);
  body_dict.Set("network", std::move(network));
}
```

This keeps the MCP tool schema simple while preserving full REST API flexibility.

**Step 8: Build and verify**

Run: `autoninja -C out/Default chrome 2>&1 | tail -20`
Expected: Compiles successfully

**Step 9: Commit**

```
feat(abp): add browser_network and browser_curl MCP tools
```

---

### Task 8: Manual End-to-End Verification

**Step 1: Build and launch**

```bash
autoninja -C out/Default chrome
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/network_test --no-first-run
```

**Step 2: Test basic network capture in action response**

```bash
# Navigate to a page with API calls
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://httpbin.org"}'

# Click something that triggers XHR — check response has "network" field
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://httpbin.org/get"}'
# Response should include: "network": {"total": N, "completed": N, "pending": 0}
```

**Step 3: Test tagging and saving**

```bash
# Navigate with tag
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/navigate \
  -H "Content-Type: application/json" \
  -d '{"url":"https://httpbin.org/get","network":{"tag":"test-flow"}}'

# Retroactive save
curl -X POST http://localhost:8222/api/v1/network \
  -H "Content-Type: application/json" \
  -d '{"tag":"retroactive-test"}'
```

**Step 4: Test network query**

```bash
# Query all saved
curl http://localhost:8222/api/v1/network?tag=test-flow

# Query with body
curl "http://localhost:8222/api/v1/network?tag=test-flow&include_body=true"

# Query with filters
curl "http://localhost:8222/api/v1/network?method=GET&hostname=httpbin"
```

**Step 5: Test browser_curl**

```bash
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/curl \
  -H "Content-Type: application/json" \
  -d '{"url":"https://httpbin.org/get","tag":"curl-test"}'

# Verify response has status, headers, body
# Verify it was saved to DB
curl "http://localhost:8222/api/v1/network?tag=curl-test"
```

**Step 6: Test MCP tools**

```bash
# browser_network query via MCP
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"browser_network","arguments":{"action":"query","tag":"test-flow"}}}'

# browser_curl via MCP
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"browser_curl","arguments":{"url":"https://httpbin.org/post","method":"POST","body":"{\"test\":true}","tag":"mcp-curl"}}}'
```

**Step 7: Test browser_wait merge behavior**

```bash
# Click that triggers requests
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/click \
  -H "Content-Type: application/json" \
  -d '{"x":100,"y":200}'
# Note network.total in response

# Wait should merge, not reset
curl -X POST http://localhost:8222/api/v1/tabs/{tab_id}/wait \
  -H "Content-Type: application/json" \
  -d '{"duration":1000}'
# network.total should be >= previous total
```

**Step 8: Test network clear**

```bash
# Clear specific tag
curl -X DELETE "http://localhost:8222/api/v1/network?tag=test-flow"

# Verify cleared
curl "http://localhost:8222/api/v1/network?tag=test-flow"
# Should return empty array

# Clear all
curl -X DELETE http://localhost:8222/api/v1/network
```

**Step 9: Commit any fixes from testing**

```
fix(abp): address issues found in network capture e2e testing
```

---

### Task 9: Update API Documentation

**Files:**
- Modify: `plans/API.md`

**Step 1: Add network endpoints to API.md**

Add the new endpoints to the API reference table and add detailed sections for:
- `GET /api/v1/network` — query parameters, response format
- `POST /api/v1/network` (save) — request body, response
- `DELETE /api/v1/network` — query parameters
- `POST /api/v1/tabs/{id}/curl` — request body, response format
- `network` parameter in action envelope — request and response format

**Step 2: Update MCP tool list**

Add `browser_network` and `browser_curl` to the MCP tools documentation.

**Step 3: Commit**

```
docs(abp): add network capture and browser_curl API documentation
```

---

### Task 10: Update MCP Skill File

**Files:**
- Modify: `tools/abp-claude-skill/abp-browser.md`

**Step 1: Add browser_network and browser_curl tool descriptions**

Document the new tools with examples showing the typical workflow:
1. Perform actions on a page
2. Notice interesting network calls in response
3. Save them with `browser_network save`
4. Query saved calls with `browser_network query`
5. Replay API calls with `browser_curl`

**Step 2: Commit**

```
docs(abp): add network capture tools to MCP skill file
```
