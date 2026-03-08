// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_types.h"

namespace abp {

CapturedRequest::CapturedRequest() = default;
CapturedRequest::~CapturedRequest() = default;
CapturedRequest::CapturedRequest(CapturedRequest&&) = default;
CapturedRequest& CapturedRequest::operator=(CapturedRequest&&) = default;

NetworkConfig::NetworkConfig() = default;
NetworkConfig::~NetworkConfig() = default;

}  // namespace abp
