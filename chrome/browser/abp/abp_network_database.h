// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_NETWORK_DATABASE_H_
#define CHROME_BROWSER_ABP_ABP_NETWORK_DATABASE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"

namespace sql {
class Database;
}

// SQLite database for persisting tagged network requests captured by ABP.
// All public methods are thread-safe and post work to an internal DB thread.
// Must be created and destroyed on the UI thread.
class AbpNetworkDatabase {
 public:
  explicit AbpNetworkDatabase(const base::FilePath& session_dir);
  ~AbpNetworkDatabase();

  AbpNetworkDatabase(const AbpNetworkDatabase&) = delete;
  AbpNetworkDatabase& operator=(const AbpNetworkDatabase&) = delete;

  // Save captured requests with a tag. Deduplicates by (tab_id, request_id, tag).
  void SaveRequests(const std::string& tag,
                    const std::string& tab_id,
                    const std::vector<abp::CapturedRequest>& requests,
                    base::OnceClosure callback);

  // Query saved requests with optional filters.
  // The *_regex fields use SQLite LIKE with %pattern% wrapping.
  struct QueryFilter {
    QueryFilter();
    ~QueryFilter();
    QueryFilter(const QueryFilter&);
    QueryFilter& operator=(const QueryFilter&);

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

  // Clear saved requests, optionally filtered by tag (empty = clear all).
  void ClearRequests(const std::string& tag, base::OnceClosure callback);

 private:
  void InitializeOnDB();
  void CreateTables();

  void SaveRequestsOnDB(const std::string& tag,
                        const std::string& tab_id,
                        std::vector<abp::CapturedRequest> requests);
  base::Value::List QueryRequestsOnDB(const QueryFilter& filter);
  void ClearRequestsOnDB(const std::string& tag);

  base::FilePath db_path_;
  std::unique_ptr<sql::Database> db_;
  scoped_refptr<base::SequencedTaskRunner> db_task_runner_;

  SEQUENCE_CHECKER(sequence_checker_);
};

#endif  // CHROME_BROWSER_ABP_ABP_NETWORK_DATABASE_H_
