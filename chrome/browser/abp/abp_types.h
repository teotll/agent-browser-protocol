// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_TYPES_H_
#define CHROME_BROWSER_ABP_ABP_TYPES_H_

#include <map>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace abp {

// Standard response callback for all ABP components.
// Used by AbpController, AbpMcpHandler, AbpHistoryController, and AbpActionContext.
//
// Parameters:
//   status: HTTP status code (200, 400, 404, 500, etc.)
//   content_type: MIME type ("application/json", "image/webp", etc.)
//   body: Response body content
using ResponseCallback = base::OnceCallback<void(int status,
                                                  const std::string& content_type,
                                                  std::string body)>;

// Response callback with custom headers support.
// Used by AbpMcpHandler for MCP protocol compliance (Mcp-Session-Id header).
//
// Parameters:
//   status: HTTP status code (200, 400, 404, 500, etc.)
//   content_type: MIME type ("application/json", "text/event-stream", etc.)
//   headers: Custom HTTP headers to include in response
//   body: Response body content
using ResponseWithHeadersCallback = base::OnceCallback<void(
    int status,
    const std::string& content_type,
    std::map<std::string, std::string> headers,
    std::string body)>;

// Network capture types
struct CapturedRequest {
  CapturedRequest();
  ~CapturedRequest();
  CapturedRequest(const CapturedRequest&);
  CapturedRequest& operator=(const CapturedRequest&);
  CapturedRequest(CapturedRequest&&);
  CapturedRequest& operator=(CapturedRequest&&);

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
  NetworkConfig();
  ~NetworkConfig();

  std::string tag;  // Empty = don't persist
  std::set<std::string> types = {"XHR", "Fetch", "Document"};  // CDP resource types
};

// Filter for querying captured network requests (used by both buffer and DB).
struct NetworkQueryFilter {
  NetworkQueryFilter();
  ~NetworkQueryFilter();
  NetworkQueryFilter(const NetworkQueryFilter&);
  NetworkQueryFilter& operator=(const NetworkQueryFilter&);

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

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_TYPES_H_
