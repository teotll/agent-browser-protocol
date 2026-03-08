// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_CURL_HANDLER_H_
#define CHROME_BROWSER_ABP_ABP_CURL_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace content {
class StoragePartition;
}

namespace network {
class SimpleURLLoader;
}

class AbpCurlHandler {
 public:
  struct Request {
    Request();
    ~Request();
    Request(const Request&);
    Request& operator=(const Request&);

    std::string url;
    std::string method = "GET";
    std::map<std::string, std::string> headers;
    std::string body;
    std::string origin;   // Auto-populated from tab's current URL
    std::string referer;  // Auto-populated from tab's current URL
  };

  struct Response {
    Response();
    ~Response();
    Response(Response&&);
    Response& operator=(Response&&);

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
                          std::string original_url,
                          ResponseCallback callback,
                          std::optional<std::string> body);

  base::WeakPtrFactory<AbpCurlHandler> weak_factory_{this};
};

#endif  // CHROME_BROWSER_ABP_ABP_CURL_HANDLER_H_
