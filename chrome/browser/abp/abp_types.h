// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_TYPES_H_
#define CHROME_BROWSER_ABP_ABP_TYPES_H_

#include <map>
#include <string>

#include "base/functional/callback.h"

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

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_TYPES_H_
