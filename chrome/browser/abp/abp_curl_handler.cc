// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_curl_handler.h"

#include <optional>
#include <string_view>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "content/public/browser/storage_partition.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace {

// Max response body — SimpleURLLoader caps DownloadToString at 5 MB.
constexpr size_t kMaxBodySize = 5 * 1024 * 1024;

bool IsBinaryContentType(const std::string& content_type) {
  // Treat anything that isn't text/* or a known text-based format as binary.
  if (content_type.empty()) {
    return false;
  }
  const std::string lower = base::ToLowerASCII(content_type);
  if (base::StartsWith(lower, "text/", base::CompareCase::SENSITIVE)) {
    return false;
  }
  if (base::StartsWith(lower, "application/json",
                       base::CompareCase::SENSITIVE)) {
    return false;
  }
  if (base::StartsWith(lower, "application/xml",
                       base::CompareCase::SENSITIVE)) {
    return false;
  }
  if (base::StartsWith(lower, "application/javascript",
                       base::CompareCase::SENSITIVE)) {
    return false;
  }
  if (base::StartsWith(lower, "application/x-www-form-urlencoded",
                       base::CompareCase::SENSITIVE)) {
    return false;
  }
  return true;
}

}  // namespace

// AbpCurlHandler::Request
AbpCurlHandler::Request::Request() = default;
AbpCurlHandler::Request::~Request() = default;
AbpCurlHandler::Request::Request(const Request&) = default;
AbpCurlHandler::Request& AbpCurlHandler::Request::operator=(const Request&) =
    default;

// AbpCurlHandler::Response
AbpCurlHandler::Response::Response() = default;
AbpCurlHandler::Response::~Response() = default;
AbpCurlHandler::Response::Response(Response&&) = default;
AbpCurlHandler::Response& AbpCurlHandler::Response::operator=(Response&&) =
    default;

// AbpCurlHandler
AbpCurlHandler::AbpCurlHandler() = default;
AbpCurlHandler::~AbpCurlHandler() = default;

void AbpCurlHandler::Execute(const Request& request,
                             content::StoragePartition* storage_partition,
                             ResponseCallback callback) {
  DCHECK(storage_partition);

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("abp_curl_handler", R"(
        semantics {
          sender: "Agent Browser Protocol"
          description:
            "ABP curl handler issues HTTP requests on behalf of an AI agent "
            "controller. The request URL, method, headers, and body are "
            "specified by the agent. Requests use the tab's storage partition "
            "so they share cookies and session state with the page."
          trigger:
            "An AI agent sends a POST /api/v1/tabs/{id}/curl or equivalent "
            "MCP tool call to the ABP HTTP server."
          data:
            "Request URL, method, headers, and optional body as supplied by "
            "the AI agent. Response body (up to 50 MB) is returned to the "
            "agent."
          destination: WEBSITE
        }
        policy {
          cookies_allowed: YES
          cookies_store: "user"
          setting:
            "This feature is only active when Chrome is launched with the "
            "--abp-port flag for AI agent control."
          policy_exception_justification:
            "ABP is a developer/AI-agent tool launched explicitly by the user. "
            "It is not part of the standard Chrome browser experience."
        })");

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(request.url);
  resource_request->method = request.method;
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kInclude;

  if (!request.referer.empty()) {
    resource_request->referrer = GURL(request.referer);
  }

  for (const auto& [name, value] : request.headers) {
    resource_request->headers.SetHeader(name, value);
  }

  // Set Origin header if provided and not already set by caller.
  if (!request.origin.empty() &&
      !resource_request->headers.HasHeader("Origin")) {
    resource_request->headers.SetHeader("Origin", request.origin);
  }

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       traffic_annotation);

  // Return body even for non-2xx responses.
  loader->SetAllowHttpErrorResults(true);

  if (!request.body.empty()) {
    // Determine content-type from request headers; fall back to octet-stream.
    std::string content_type = "application/octet-stream";
    for (const auto& [name, value] : request.headers) {
      if (base::EqualsCaseInsensitiveASCII(name, "content-type")) {
        content_type = value;
        break;
      }
    }
    loader->AttachStringForUpload(request.body, content_type);
  }

  // Keep a raw pointer before moving into the callback.
  network::SimpleURLLoader* loader_ptr = loader.get();
  std::string original_url = request.url;

  scoped_refptr<network::SharedURLLoaderFactory> factory =
      storage_partition->GetURLLoaderFactoryForBrowserProcess();

  loader_ptr->DownloadToString(
      factory.get(),
      base::BindOnce(&AbpCurlHandler::OnResponseReceived,
                     weak_factory_.GetWeakPtr(), std::move(loader),
                     std::move(original_url), std::move(callback)),
      kMaxBodySize);
}

void AbpCurlHandler::OnResponseReceived(
    std::unique_ptr<network::SimpleURLLoader> loader,
    std::string original_url,
    ResponseCallback callback,
    std::optional<std::string> body) {
  Response response;

  // Populate final URL and detect redirects.
  response.final_url = loader->GetFinalURL().spec();
  response.redirected = (GURL(original_url) != loader->GetFinalURL());

  const network::mojom::URLResponseHead* response_info = loader->ResponseInfo();
  if (response_info && response_info->headers) {
    const net::HttpResponseHeaders* http_headers =
        response_info->headers.get();

    response.status_code = http_headers->response_code();

    // Enumerate all response headers.
    size_t iter = 0;
    std::string header_name;
    std::string header_value;
    while (
        http_headers->EnumerateHeaderLines(&iter, &header_name, &header_value)) {
      // Last value wins for duplicate headers (simple map).
      response.headers[header_name] = header_value;
    }
  } else {
    // Network error — use net error code as a pseudo status.
    response.status_code = loader->NetError();
  }

  if (body.has_value() && !body->empty()) {
    // Determine content-type to decide whether to base64-encode.
    std::string content_type;
    for (const auto& [name, value] : response.headers) {
      if (base::EqualsCaseInsensitiveASCII(name, "content-type")) {
        content_type = value;
        break;
      }
    }

    if (IsBinaryContentType(content_type)) {
      response.body = base::Base64Encode(*body);
      response.body_is_base64 = true;
    } else {
      response.body = std::move(*body);
    }
  }

  VLOG(1) << "ABP curl: " << original_url << " -> " << response.status_code
          << " (redirected=" << response.redirected
          << ", body=" << response.body.size() << "B)";

  std::move(callback).Run(std::move(response));
}
