// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_network_capture.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_controller.h"
#include "url/gurl.h"

namespace {
// 50 MB limit for response bodies.
constexpr size_t kMaxBodySize = 50 * 1024 * 1024;
}  // namespace

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
  request_index_.clear();
  cors_preflight_ids_.clear();
  service_worker_request_ids_.clear();
}

abp::CapturedRequest* AbpNetworkCapture::FindRequest(
    const std::string& request_id) {
  auto it = request_index_.find(request_id);
  if (it == request_index_.end()) {
    return nullptr;
  }
  return &requests_[it->second];
}

int AbpNetworkCapture::GetTotalCount() const {
  return static_cast<int>(requests_.size());
}

int AbpNetworkCapture::GetCompletedCount() const {
  int count = 0;
  for (const auto& req : requests_) {
    if (req.completed) {
      ++count;
    }
  }
  return count;
}

int AbpNetworkCapture::GetPendingCount() const {
  int count = 0;
  for (const auto& req : requests_) {
    if (!req.completed) {
      ++count;
    }
  }
  return count;
}

void AbpNetworkCapture::MarkCorsPreflight(const std::string& request_id) {
  abp::CapturedRequest* req = FindRequest(request_id);
  if (req) {
    req->cors_preflight = true;
  }
}

void AbpNetworkCapture::OnNetworkEvent(const std::string& method,
                                       const base::Value::Dict& params,
                                       abp::AbpCdpClient* cdp_client) {
  if (method == "Network.requestWillBeSent") {
    OnRequestWillBeSent(params);
  } else if (method == "Network.responseReceived") {
    OnResponseReceived(params);
  } else if (method == "Network.loadingFinished") {
    OnLoadingFinished(params, cdp_client);
  } else if (method == "Network.loadingFailed") {
    OnLoadingFailed(params);
  } else if (method == "Network.requestWillBeSentExtraInfo") {
    OnRequestWillBeSentExtraInfo(params);
  } else if (method == "Network.responseReceivedExtraInfo") {
    OnResponseReceivedExtraInfo(params);
  } else if (method == "Network.requestServedFromServiceWorker") {
    OnRequestServedFromServiceWorker(params);
  }
}

bool AbpNetworkCapture::PassesTypeFilter(
    const std::string& resource_type) const {
  if (capture_types_.empty()) {
    return true;
  }
  // Case-insensitive match against configured types.
  // CDP uses mixed case (e.g. "XHR", "Fetch", "Document").
  for (const std::string& allowed : capture_types_) {
    if (base::EqualsCaseInsensitiveASCII(allowed, resource_type)) {
      return true;
    }
  }
  return false;
}

void AbpNetworkCapture::RemoveAtIndex(size_t index) {
  const std::string& id_to_remove = requests_[index].request_id;
  request_index_.erase(id_to_remove);

  size_t last = requests_.size() - 1;
  if (index != last) {
    // Swap with last element and update its index.
    requests_[index] = std::move(requests_[last]);
    request_index_[requests_[index].request_id] = index;
  }
  requests_.pop_back();
}

void AbpNetworkCapture::EvictOldestIfFull() {
  if (requests_.size() < kMaxBufferSize) {
    return;
  }

  // Remove the oldest entry (index 0) using RemoveAtIndex.
  // This uses swap-with-last which doesn't preserve insertion order for the
  // evicted slot, but the oldest entry is reliably removed.
  // Since we need to evict index 0 specifically, use the erase+rebuild path
  // to preserve ordering guarantees.
  const std::string removed_id = requests_[0].request_id;
  request_index_.erase(removed_id);
  requests_.erase(requests_.begin());

  // Rebuild the index — O(n) but acceptable for a 1000-item cap.
  request_index_.clear();
  for (size_t i = 0; i < requests_.size(); ++i) {
    request_index_[requests_[i].request_id] = i;
  }
}

void AbpNetworkCapture::OnRequestWillBeSent(const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  const base::Value::Dict* request_dict = params.FindDict("request");
  const std::string* method_str =
      request_dict ? request_dict->FindString("method") : nullptr;

  // OPTIONS requests are CORS preflights — track them but don't buffer.
  if (method_str && *method_str == "OPTIONS") {
    cors_preflight_ids_.insert(*request_id);
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
    hop.Set("url", req->url);
    if (auto status = redirect_response->FindInt("status")) {
      hop.Set("status", *status);
    }
    if (const std::string* status_text =
            redirect_response->FindString("statusText")) {
      hop.Set("status_text", *status_text);
    }
    req->redirect_chain.Append(std::move(hop));

    // Update URL and method to the redirect destination.
    if (request_dict) {
      if (const std::string* url = request_dict->FindString("url")) {
        req->url = *url;
        GURL gurl(*url);
        req->url_hostname = gurl.host();
        req->url_path = gurl.path();
        req->url_query = gurl.query();
      }
      if (method_str) {
        req->method = *method_str;
      }
    }
    return;
  }

  // Skip streaming / long-lived connection types.
  const std::string* type = params.FindString("type");
  if (type && (*type == "WebSocket" || *type == "EventSource" ||
               *type == "Ping" || *type == "Prefetch" ||
               *type == "CSPViolationReport")) {
    return;
  }

  // Apply resource type filter for known types.
  std::string resource_type = type ? *type : std::string();
  if (!resource_type.empty() && !PassesTypeFilter(resource_type)) {
    return;
  }

  // New request — evict oldest if at capacity.
  EvictOldestIfFull();

  abp::CapturedRequest new_req;
  new_req.request_id = *request_id;
  new_req.action_id = current_action_id_;
  new_req.resource_type = resource_type;
  new_req.started_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Request details
  if (request_dict) {
    if (const std::string* url = request_dict->FindString("url")) {
      new_req.url = *url;
      GURL gurl(*url);
      new_req.url_hostname = gurl.host();
      new_req.url_path = gurl.path();
      new_req.url_query = gurl.query();
    }
    if (method_str) {
      new_req.method = *method_str;
    }
    // Request headers
    if (const base::Value::Dict* headers = request_dict->FindDict("headers")) {
      new_req.request_headers = headers->Clone();
    }
    // Request body (postData)
    if (const std::string* post_data = request_dict->FindString("postData")) {
      new_req.request_body = *post_data;
    }
  }

  // Service worker flag from requestWillBeSent
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

  // Update resource_type if it was unknown at request time.
  const std::string* type = params.FindString("type");
  if (type && req->resource_type.empty()) {
    req->resource_type = *type;
  }

  // Late-filter: now that we know the type, remove if it doesn't match.
  // req->resource_type is now up-to-date (set above if it was empty).
  if (!req->resource_type.empty() && !PassesTypeFilter(req->resource_type)) {
    // Find and remove this entry using swap-with-last.
    auto it = request_index_.find(*request_id);
    if (it != request_index_.end()) {
      RemoveAtIndex(it->second);
    }
    return;
  }

  // Mark CORS preflight if the request_id was an OPTIONS request.
  if (cors_preflight_ids_.count(*request_id)) {
    req->cors_preflight = true;
  }

  if (auto sw = response->FindBool("fromServiceWorker")) {
    req->served_from_service_worker = *sw;
  }
}

void AbpNetworkCapture::OnLoadingFinished(const base::Value::Dict& params,
                                          abp::AbpCdpClient* cdp_client) {
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

  // Eagerly fetch response body via CDP.
  if (cdp_client) {
    base::Value::Dict body_params;
    body_params.Set("requestId", *request_id);
    cdp_client->SendCommand(
        "Network.getResponseBody", body_params,
        base::BindOnce(&AbpNetworkCapture::OnResponseBodyReceived,
                       weak_factory_.GetWeakPtr(), *request_id));
  }
}

void AbpNetworkCapture::OnResponseBodyReceived(const std::string& request_id,
                                                bool success,
                                                const std::string& response) {
  if (!success) {
    return;
  }

  abp::CapturedRequest* req = FindRequest(request_id);
  if (!req) {
    return;
  }

  // Parse JSON: {"body": "...", "base64Encoded": bool}
  auto parsed = base::JSONReader::ReadDict(response, 0);
  if (!parsed) {
    return;
  }

  const std::string* body = parsed->FindString("body");
  if (!body) {
    return;
  }

  // Skip bodies larger than 50 MB.
  if (body->size() > kMaxBodySize) {
    return;
  }

  req->response_body = *body;

  if (auto is_base64 = parsed->FindBool("base64Encoded")) {
    req->response_body_is_base64 = *is_base64;
  }
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

void AbpNetworkCapture::OnRequestServedFromServiceWorker(
    const base::Value::Dict& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  service_worker_request_ids_.insert(*request_id);

  // Remove from buffer if already captured — not needed for SW-served requests.
  auto it = request_index_.find(*request_id);
  if (it != request_index_.end()) {
    RemoveAtIndex(it->second);
  }
}
