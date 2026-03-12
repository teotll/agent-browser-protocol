// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_types.h"

namespace abp {

CapturedRequest::CapturedRequest() = default;
CapturedRequest::~CapturedRequest() = default;

CapturedRequest::CapturedRequest(const CapturedRequest& other)
    : request_id(other.request_id),
      action_id(other.action_id),
      url(other.url),
      url_hostname(other.url_hostname),
      url_path(other.url_path),
      url_query(other.url_query),
      method(other.method),
      request_headers(other.request_headers.Clone()),
      request_body(other.request_body),
      resource_type(other.resource_type),
      cors_preflight(other.cors_preflight),
      status_code(other.status_code),
      response_headers(other.response_headers.Clone()),
      response_body(other.response_body),
      response_body_is_base64(other.response_body_is_base64),
      redirect_chain(other.redirect_chain.Clone()),
      started_at_ms(other.started_at_ms),
      completed_at_ms(other.completed_at_ms),
      virtual_time_ms(other.virtual_time_ms),
      completed(other.completed),
      served_from_service_worker(other.served_from_service_worker) {}

CapturedRequest& CapturedRequest::operator=(const CapturedRequest& other) {
  if (this != &other) {
    request_id = other.request_id;
    action_id = other.action_id;
    url = other.url;
    url_hostname = other.url_hostname;
    url_path = other.url_path;
    url_query = other.url_query;
    method = other.method;
    request_headers = other.request_headers.Clone();
    request_body = other.request_body;
    resource_type = other.resource_type;
    cors_preflight = other.cors_preflight;
    status_code = other.status_code;
    response_headers = other.response_headers.Clone();
    response_body = other.response_body;
    response_body_is_base64 = other.response_body_is_base64;
    redirect_chain = other.redirect_chain.Clone();
    started_at_ms = other.started_at_ms;
    completed_at_ms = other.completed_at_ms;
    virtual_time_ms = other.virtual_time_ms;
    completed = other.completed;
    served_from_service_worker = other.served_from_service_worker;
  }
  return *this;
}

CapturedRequest::CapturedRequest(CapturedRequest&&) = default;
CapturedRequest& CapturedRequest::operator=(CapturedRequest&&) = default;

NetworkConfig::NetworkConfig() = default;
NetworkConfig::~NetworkConfig() = default;

NetworkQueryFilter::NetworkQueryFilter() = default;
NetworkQueryFilter::~NetworkQueryFilter() = default;
NetworkQueryFilter::NetworkQueryFilter(const NetworkQueryFilter&) = default;
NetworkQueryFilter& NetworkQueryFilter::operator=(const NetworkQueryFilter&) =
    default;

ConsoleEntry::ConsoleEntry() = default;
ConsoleEntry::~ConsoleEntry() = default;
ConsoleEntry::ConsoleEntry(const ConsoleEntry&) = default;
ConsoleEntry& ConsoleEntry::operator=(const ConsoleEntry&) = default;
ConsoleEntry::ConsoleEntry(ConsoleEntry&&) = default;
ConsoleEntry& ConsoleEntry::operator=(ConsoleEntry&&) = default;

base::Value::Dict ConsoleEntry::ToDict() const {
  base::Value::Dict dict;
  dict.Set("id", static_cast<double>(id));
  dict.Set("tab_id", tab_id);
  dict.Set("level", level);
  dict.Set("message", message);
  dict.Set("line_number", line_number);
  dict.Set("source_url", source_url);
  dict.Set("stack_trace", stack_trace);
  dict.Set("timestamp_ms", static_cast<double>(timestamp_ms));
  return dict;
}

}  // namespace abp
