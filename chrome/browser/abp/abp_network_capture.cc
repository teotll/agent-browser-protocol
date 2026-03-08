// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_network_capture.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "url/gurl.h"

AbpNetworkCapture::AbpNetworkCapture() = default;
AbpNetworkCapture::~AbpNetworkCapture() = default;

void AbpNetworkCapture::EnableCapture(const abp::NetworkConfig& config) {
  enabled_ = true;
  config_ = config;
}

void AbpNetworkCapture::DisableCapture() {
  enabled_ = false;
  Clear();
}

void AbpNetworkCapture::Clear() {
  requests_.clear();
  request_index_.clear();
}

abp::CapturedRequest* AbpNetworkCapture::FindRequest(
    const std::string& request_id) {
  auto it = request_index_.find(request_id);
  if (it == request_index_.end()) {
    return nullptr;
  }
  return &requests_[it->second];
}

void AbpNetworkCapture::OnCdpNetworkEvent(const std::string& method,
                                          const base::Value::Dict& params,
                                          const std::string& current_action_id) {
  if (!enabled_) {
    return;
  }

  if (method == "Network.requestWillBeSent") {
    OnRequestWillBeSent(params, current_action_id);
  } else if (method == "Network.responseReceived") {
    OnResponseReceived(params);
  } else if (method == "Network.loadingFinished") {
    OnLoadingFinished(params);
  } else if (method == "Network.loadingFailed") {
    OnLoadingFailed(params);
  } else if (method == "Network.requestWillBeSentExtraInfo") {
    OnRequestWillBeSentExtraInfo(params);
  } else if (method == "Network.responseReceivedExtraInfo") {
    OnResponseReceivedExtraInfo(params);
  }
}

bool AbpNetworkCapture::PassesTypeFilter(
    const std::string& resource_type) const {
  if (config_.types.empty()) {
    return true;
  }
  // Case-insensitive match against configured types.
  // CDP uses mixed case (e.g. "XHR", "Fetch", "Document").
  for (const std::string& allowed : config_.types) {
    if (base::EqualsCaseInsensitiveASCII(allowed, resource_type)) {
      return true;
    }
  }
  return false;
}

void AbpNetworkCapture::EvictOldestIfFull() {
  if (requests_.size() < kMaxRequests) {
    return;
  }

  // Remove the oldest entry (index 0) and shift all remaining entries down.
  requests_.erase(requests_.begin());

  // Rebuild the index from scratch — O(n) but acceptable for 1000-item cap.
  request_index_.clear();
  for (size_t i = 0; i < requests_.size(); ++i) {
    request_index_[requests_[i].request_id] = i;
  }
}

void AbpNetworkCapture::OnRequestWillBeSent(const base::Value::Dict& params,
                                            const std::string& action_id) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  // Skip streaming / long-lived connection types.
  const std::string* type = params.FindString("type");
  if (type && (*type == "WebSocket" || *type == "EventSource" ||
               *type == "Ping" || *type == "Prefetch" ||
               *type == "CSPViolationReport")) {
    return;
  }

  // Apply resource type filter.
  std::string resource_type = type ? *type : std::string();
  if (!PassesTypeFilter(resource_type)) {
    return;
  }

  // Check for redirect chain (redirectResponse present means this is a
  // redirect follow-up for an existing request).
  const base::Value::Dict* redirect_response =
      params.FindDict("redirectResponse");

  abp::CapturedRequest* req = FindRequest(*request_id);
  if (req && redirect_response) {
    // Append the redirect hop to the chain and update the URL.
    base::Value::Dict hop;
    if (const std::string* status_text =
            redirect_response->FindString("statusText")) {
      hop.Set("status_text", *status_text);
    }
    if (auto status = redirect_response->FindInt("status")) {
      hop.Set("status", *status);
    }
    if (const std::string* prev_url = params.FindString("documentURL")) {
      // The redirect target URL — populate before overwrite.
      (void)prev_url;
    }
    // The prior URL is now stored in the existing req->url.
    hop.Set("url", req->url);
    req->redirect_chain.Append(std::move(hop));

    // Update URL to the new destination.
    const base::Value::Dict* request_dict = params.FindDict("request");
    if (request_dict) {
      if (const std::string* url = request_dict->FindString("url")) {
        req->url = *url;
        GURL gurl(*url);
        req->url_hostname = gurl.host();
        req->url_path = gurl.path();
        req->url_query = gurl.query();
      }
      if (const std::string* method = request_dict->FindString("method")) {
        req->method = *method;
      }
    }
    return;
  }

  // New request — evict oldest if at capacity.
  EvictOldestIfFull();

  abp::CapturedRequest new_req;
  new_req.request_id = *request_id;
  new_req.action_id = action_id;
  new_req.resource_type = resource_type;

  // Timestamps
  if (auto ts = params.FindDouble("timestamp")) {
    // CDP timestamp is seconds since process start — convert to ms.
    new_req.started_at_ms = static_cast<int64_t>(*ts * 1000.0);
  }
  new_req.started_at_ms =
      base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Request details
  const base::Value::Dict* request_dict = params.FindDict("request");
  if (request_dict) {
    if (const std::string* url = request_dict->FindString("url")) {
      new_req.url = *url;
      GURL gurl(*url);
      new_req.url_hostname = gurl.host();
      new_req.url_path = gurl.path();
      new_req.url_query = gurl.query();
    }
    if (const std::string* method = request_dict->FindString("method")) {
      new_req.method = *method;
    }
    // Request headers
    if (const base::Value::Dict* headers = request_dict->FindDict("headers")) {
      new_req.request_headers = headers->Clone();
    }
    // Request body (postData)
    if (const std::string* post_data = request_dict->FindString("postData")) {
      new_req.request_body = *post_data;
    }
    // CORS preflight detection
    if (new_req.method == "OPTIONS") {
      if (new_req.request_headers.FindString("Access-Control-Request-Method") ||
          new_req.request_headers.FindString(
              "access-control-request-method")) {
        new_req.cors_preflight = true;
      }
    }
  }

  // Service worker
  if (auto sw = params.FindBool("fromServiceWorker")) {
    new_req.served_from_service_worker = *sw;
  }

  size_t idx = requests_.size();
  request_index_[new_req.request_id] = idx;
  requests_.push_back(std::move(new_req));
}

void AbpNetworkCapture::OnResponseReceived(const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  abp::CapturedRequest* req = FindRequest(*request_id);
  if (!req) {
    return;
  }

  const base::Value::Dict* response = params.FindDict("response");
  if (!response) {
    return;
  }

  if (auto status = response->FindInt("status")) {
    req->status_code = *status;
  }
  if (const base::Value::Dict* headers = response->FindDict("headers")) {
    req->response_headers = headers->Clone();
  }
  if (auto sw = response->FindBool("fromServiceWorker")) {
    req->served_from_service_worker = *sw;
  }
}

void AbpNetworkCapture::OnLoadingFinished(const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  abp::CapturedRequest* req = FindRequest(*request_id);
  if (!req) {
    return;
  }

  req->completed = true;
  req->completed_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
}

void AbpNetworkCapture::OnLoadingFailed(const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  abp::CapturedRequest* req = FindRequest(*request_id);
  if (!req) {
    return;
  }

  req->completed = true;
  req->completed_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  // Preserve status_code = 0 to indicate network failure (not HTTP error).
}

void AbpNetworkCapture::OnRequestWillBeSentExtraInfo(
    const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  abp::CapturedRequest* req = FindRequest(*request_id);
  if (!req) {
    return;
  }

  // Merge extra request headers (may include cookies, sec-fetch-* etc.)
  if (const base::Value::Dict* headers = params.FindDict("headers")) {
    for (const auto [key, val] : *headers) {
      if (val.is_string()) {
        req->request_headers.Set(key, val.GetString());
      }
    }
  }
}

void AbpNetworkCapture::OnResponseReceivedExtraInfo(
    const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  abp::CapturedRequest* req = FindRequest(*request_id);
  if (!req) {
    return;
  }

  // Prefer extra-info headers (includes set-cookie etc. that are
  // redacted in responseReceived).
  if (const base::Value::Dict* headers = params.FindDict("headers")) {
    req->response_headers = headers->Clone();
  }
  if (auto status = params.FindInt("statusCode")) {
    req->status_code = *status;
  }
}
