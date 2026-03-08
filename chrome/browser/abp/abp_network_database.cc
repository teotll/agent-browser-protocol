// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_network_database.h"

#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "sql/transaction.h"

AbpNetworkDatabase::QueryFilter::QueryFilter() = default;
AbpNetworkDatabase::QueryFilter::~QueryFilter() = default;
AbpNetworkDatabase::QueryFilter::QueryFilter(const QueryFilter&) = default;
AbpNetworkDatabase::QueryFilter& AbpNetworkDatabase::QueryFilter::operator=(
    const QueryFilter&) = default;

namespace {

constexpr char kCreateUrlIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_url ON network_requests(url)";
constexpr char kCreateHostnameIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_hostname ON "
    "network_requests(url_hostname)";
constexpr char kCreatePathIndex[] =
    "CREATE INDEX IF NOT EXISTS idx_nr_path ON network_requests(url_path)";

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

// Serialize a base::Value::Dict to a JSON string. Returns "" on failure.
std::string DictToJson(const base::Value::Dict& dict) {
  std::string json;
  base::JSONWriter::Write(dict, &json);
  return json;
}

// Serialize a base::Value::List to a JSON string. Returns "" on failure.
std::string ListToJson(const base::Value::List& list) {
  std::string json;
  base::JSONWriter::Write(list, &json);
  return json;
}

// Bind a string that may be empty as NULL if empty.
void BindStringOrNull(sql::Statement& stmt, int col, const std::string& s) {
  if (s.empty()) {
    stmt.BindNull(col);
  } else {
    stmt.BindString(col, s);
  }
}

}  // namespace

AbpNetworkDatabase::AbpNetworkDatabase(const base::FilePath& session_dir) {
  DETACH_FROM_SEQUENCE(sequence_checker_);

  db_path_ = session_dir.AppendASCII("network_requests.db");

  db_task_runner_ = base::ThreadPool::CreateSequencedTaskRunner(
      {base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN, base::MayBlock()});

  // Initialize the database on the DB thread.
  db_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&AbpNetworkDatabase::InitializeOnDB,
                                base::Unretained(this)));
}

AbpNetworkDatabase::~AbpNetworkDatabase() {
  if (db_) {
    db_task_runner_->DeleteSoon(FROM_HERE, std::move(db_));
  }
}

void AbpNetworkDatabase::SaveRequests(
    const std::string& tag,
    const std::string& tab_id,
    const std::vector<abp::CapturedRequest>& requests,
    base::OnceClosure callback) {
  // Copy the requests so they can be moved to the DB thread.
  std::vector<abp::CapturedRequest> requests_copy(requests.begin(),
                                                   requests.end());

  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::SaveRequestsOnDB,
                     base::Unretained(this), tag, tab_id,
                     std::move(requests_copy)),
      std::move(callback));
}

void AbpNetworkDatabase::QueryRequests(const QueryFilter& filter,
                                       QueryCallback callback) {
  db_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::QueryRequestsOnDB,
                     base::Unretained(this), filter),
      std::move(callback));
}

void AbpNetworkDatabase::ClearRequests(const std::string& tag,
                                       base::OnceClosure callback) {
  db_task_runner_->PostTaskAndReply(
      FROM_HERE,
      base::BindOnce(&AbpNetworkDatabase::ClearRequestsOnDB,
                     base::Unretained(this), tag),
      std::move(callback));
}

// --- DB thread methods ---

void AbpNetworkDatabase::InitializeOnDB() {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());

  // Ensure parent directory exists.
  base::FilePath dir = db_path_.DirName();
  if (!base::DirectoryExists(dir)) {
    if (!base::CreateDirectory(dir)) {
      LOG(ERROR) << "ABP: Failed to create network database directory: " << dir;
      return;
    }
  }

  db_ = std::make_unique<sql::Database>(
      sql::DatabaseOptions()
          .set_wal_mode(true)
          .set_exclusive_locking(false),
      sql::Database::Tag("ABP"));

  if (!db_->Open(db_path_)) {
    LOG(ERROR) << "ABP: Failed to open network database: " << db_path_;
    db_.reset();
    return;
  }

  CreateTables();

  VLOG(1) << "ABP: Network database initialized at " << db_path_;
}

void AbpNetworkDatabase::CreateTables() {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  DCHECK(db_);

  sql::Transaction transaction(db_.get());
  if (!transaction.Begin()) {
    LOG(ERROR) << "ABP: Failed to begin transaction for network table creation";
    return;
  }

  if (!db_->Execute(kCreateNetworkRequestsTable)) {
    LOG(ERROR) << "ABP: Failed to create network_requests table";
    return;
  }

  // Indexes for common query patterns.
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_nr_tag ON network_requests(tag)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_nr_tab_id ON network_requests(tab_id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_nr_action_id ON "
      "network_requests(action_id)");
  std::ignore = db_->Execute(
      "CREATE INDEX IF NOT EXISTS idx_nr_tag_tab ON "
      "network_requests(tag, tab_id)");
  std::ignore = db_->Execute(kCreateUrlIndex);
  std::ignore = db_->Execute(kCreateHostnameIndex);
  std::ignore = db_->Execute(kCreatePathIndex);

  if (!transaction.Commit()) {
    LOG(ERROR) << "ABP: Failed to commit network table creation transaction";
  }
}

void AbpNetworkDatabase::SaveRequestsOnDB(
    const std::string& tag,
    const std::string& tab_id,
    std::vector<abp::CapturedRequest> requests) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return;
  }

  sql::Transaction transaction(db_.get());
  if (!transaction.Begin()) {
    LOG(ERROR) << "ABP: Failed to begin transaction for SaveRequests";
    return;
  }

  sql::Statement stmt(db_->GetCachedStatement(
      SQL_FROM_HERE,
      "INSERT OR IGNORE INTO network_requests ("
      "  request_id, action_id, tab_id, tag, url, url_hostname, url_path,"
      "  url_query, method, request_headers, request_body, resource_type,"
      "  cors_preflight, status, response_headers, response_body,"
      "  response_body_encoding, redirect_chain, started_at_ms,"
      "  completed_at_ms, duration_ms, virtual_time_ms"
      ") VALUES ("
      "  ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?"
      ")"));

  for (const auto& req : requests) {
    stmt.Reset(true);

    stmt.BindString(0, req.request_id);
    stmt.BindString(1, req.action_id);
    stmt.BindString(2, tab_id);
    stmt.BindString(3, tag);
    stmt.BindString(4, req.url);
    stmt.BindString(5, req.url_hostname);
    stmt.BindString(6, req.url_path);
    BindStringOrNull(stmt, 7, req.url_query);
    stmt.BindString(8, req.method);

    // Serialize dict/list fields to JSON.
    std::string req_headers_json = DictToJson(req.request_headers);
    BindStringOrNull(stmt, 9, req_headers_json);
    BindStringOrNull(stmt, 10, req.request_body);
    BindStringOrNull(stmt, 11, req.resource_type);
    stmt.BindInt(12, req.cors_preflight ? 1 : 0);

    if (req.status_code != 0) {
      stmt.BindInt(13, req.status_code);
    } else {
      stmt.BindNull(13);
    }

    std::string resp_headers_json = DictToJson(req.response_headers);
    BindStringOrNull(stmt, 14, resp_headers_json);
    BindStringOrNull(stmt, 15, req.response_body);

    // Store encoding flag as a string ("base64" or empty).
    if (req.response_body_is_base64) {
      stmt.BindString(16, "base64");
    } else {
      stmt.BindNull(16);
    }

    std::string redirect_chain_json = ListToJson(req.redirect_chain);
    BindStringOrNull(stmt, 17, redirect_chain_json);

    if (req.started_at_ms != 0) {
      stmt.BindInt64(18, req.started_at_ms);
    } else {
      stmt.BindNull(18);
    }

    if (req.completed_at_ms != 0) {
      stmt.BindInt64(19, req.completed_at_ms);
    } else {
      stmt.BindNull(19);
    }

    // duration_ms derived from start/complete times.
    if (req.started_at_ms != 0 && req.completed_at_ms != 0) {
      stmt.BindInt64(20, req.completed_at_ms - req.started_at_ms);
    } else {
      stmt.BindNull(20);
    }

    if (req.virtual_time_ms != 0) {
      stmt.BindInt64(21, req.virtual_time_ms);
    } else {
      stmt.BindNull(21);
    }

    if (!stmt.Run()) {
      LOG(WARNING) << "ABP: Failed to insert network request: "
                   << req.request_id;
    }
  }

  if (!transaction.Commit()) {
    LOG(ERROR) << "ABP: Failed to commit SaveRequests transaction";
  }
}

base::Value::List AbpNetworkDatabase::QueryRequestsOnDB(
    const QueryFilter& filter) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  base::Value::List results;

  if (!db_) {
    return results;
  }

  // Build WHERE clause dynamically.
  std::string where = " WHERE 1=1";
  if (!filter.tag.empty()) {
    where += " AND tag = ?";
  }
  if (!filter.tab_id.empty()) {
    where += " AND tab_id = ?";
  }
  if (!filter.action_id.empty()) {
    where += " AND action_id = ?";
  }
  if (!filter.type.empty()) {
    where += " AND resource_type = ?";
  }
  if (!filter.url_regex.empty()) {
    where += " AND url LIKE ?";
  }
  if (!filter.hostname_regex.empty()) {
    where += " AND url_hostname LIKE ?";
  }
  if (!filter.path_regex.empty()) {
    where += " AND url_path LIKE ?";
  }
  if (!filter.query_regex.empty()) {
    where += " AND url_query LIKE ?";
  }
  if (!filter.method_regex.empty()) {
    where += " AND method LIKE ?";
  }
  if (!filter.status_regex.empty()) {
    where += " AND CAST(status AS TEXT) LIKE ?";
  }

  std::string query =
      "SELECT request_id, action_id, tab_id, tag, url, url_hostname,"
      "  url_path, url_query, method, request_headers, request_body,"
      "  resource_type, cors_preflight, status, response_headers,"
      "  response_body, response_body_encoding, redirect_chain,"
      "  started_at_ms, completed_at_ms, duration_ms, virtual_time_ms"
      " FROM network_requests" +
      where + " ORDER BY id ASC";

  sql::Statement stmt(db_->GetUniqueStatement(query));

  int col = 0;
  if (!filter.tag.empty()) {
    stmt.BindString(col++, filter.tag);
  }
  if (!filter.tab_id.empty()) {
    stmt.BindString(col++, filter.tab_id);
  }
  if (!filter.action_id.empty()) {
    stmt.BindString(col++, filter.action_id);
  }
  if (!filter.type.empty()) {
    stmt.BindString(col++, filter.type);
  }
  if (!filter.url_regex.empty()) {
    stmt.BindString(col++, "%" + filter.url_regex + "%");
  }
  if (!filter.hostname_regex.empty()) {
    stmt.BindString(col++, "%" + filter.hostname_regex + "%");
  }
  if (!filter.path_regex.empty()) {
    stmt.BindString(col++, "%" + filter.path_regex + "%");
  }
  if (!filter.query_regex.empty()) {
    stmt.BindString(col++, "%" + filter.query_regex + "%");
  }
  if (!filter.method_regex.empty()) {
    stmt.BindString(col++, "%" + filter.method_regex + "%");
  }
  if (!filter.status_regex.empty()) {
    stmt.BindString(col++, "%" + filter.status_regex + "%");
  }

  while (stmt.Step()) {
    base::Value::Dict row;

    row.Set("request_id", stmt.ColumnString(0));
    row.Set("action_id", stmt.ColumnString(1));
    row.Set("tab_id", stmt.ColumnString(2));
    row.Set("tag", stmt.ColumnString(3));
    row.Set("url", stmt.ColumnString(4));
    row.Set("url_hostname", stmt.ColumnString(5));
    row.Set("url_path", stmt.ColumnString(6));
    if (stmt.GetColumnType(7) != sql::ColumnType::kNull) {
      row.Set("url_query", stmt.ColumnString(7));
    }
    row.Set("method", stmt.ColumnString(8));

    if (filter.include_body) {
      // request_headers (col 9) — only include when include_body is set.
      if (stmt.GetColumnType(9) != sql::ColumnType::kNull) {
        row.Set("request_headers", stmt.ColumnString(9));
      }
      if (stmt.GetColumnType(10) != sql::ColumnType::kNull) {
        row.Set("request_body", stmt.ColumnString(10));
      }
    }

    if (stmt.GetColumnType(11) != sql::ColumnType::kNull) {
      row.Set("resource_type", stmt.ColumnString(11));
    }

    row.Set("cors_preflight", stmt.ColumnInt(12) != 0);

    if (stmt.GetColumnType(13) != sql::ColumnType::kNull) {
      row.Set("status", stmt.ColumnInt(13));
    }

    if (filter.include_body) {
      // response_headers (col 14) — only include when include_body is set.
      if (stmt.GetColumnType(14) != sql::ColumnType::kNull) {
        row.Set("response_headers", stmt.ColumnString(14));
      }
      if (stmt.GetColumnType(15) != sql::ColumnType::kNull) {
        row.Set("response_body", stmt.ColumnString(15));
      }
      if (stmt.GetColumnType(16) != sql::ColumnType::kNull) {
        row.Set("response_body_encoding", stmt.ColumnString(16));
      }
    }

    if (stmt.GetColumnType(17) != sql::ColumnType::kNull) {
      row.Set("redirect_chain", stmt.ColumnString(17));
    }

    if (stmt.GetColumnType(18) != sql::ColumnType::kNull) {
      row.Set("started_at_ms", static_cast<double>(stmt.ColumnInt64(18)));
    }
    if (stmt.GetColumnType(19) != sql::ColumnType::kNull) {
      row.Set("completed_at_ms", static_cast<double>(stmt.ColumnInt64(19)));
    }
    if (stmt.GetColumnType(20) != sql::ColumnType::kNull) {
      row.Set("duration_ms", static_cast<double>(stmt.ColumnInt64(20)));
    }
    if (stmt.GetColumnType(21) != sql::ColumnType::kNull) {
      row.Set("virtual_time_ms", static_cast<double>(stmt.ColumnInt64(21)));
    }

    results.Append(std::move(row));
  }

  return results;
}

void AbpNetworkDatabase::ClearRequestsOnDB(const std::string& tag) {
  DCHECK(db_task_runner_->RunsTasksInCurrentSequence());
  if (!db_) {
    return;
  }

  if (tag.empty()) {
    if (!db_->Execute("DELETE FROM network_requests")) {
      LOG(ERROR) << "ABP: Failed to clear all network requests";
    }
  } else {
    sql::Statement stmt(db_->GetCachedStatement(
        SQL_FROM_HERE, "DELETE FROM network_requests WHERE tag = ?"));
    stmt.BindString(0, tag);
    if (!stmt.Run()) {
      LOG(ERROR) << "ABP: Failed to clear network requests for tag: " << tag;
    }
  }
}
