// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_
#define CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"

// Per-tab in-memory buffer that listens to CDP Network domain events and
// assembles abp::CapturedRequest records.
//
// Usage:
//   1. Call EnableCapture(config) to start capturing (sends Network.enable).
//   2. Call OnCdpNetworkEvent() for every CDP Network.* event received.
//   3. Call GetRequests() / FindRequest() to read captured data.
//   4. Call Clear() to discard all captured requests.
//
// This class is NOT in the abp namespace — it follows the same convention
// as AbpNetworkDatabase (a plain class, no namespace).
//
// Must be created and used on the UI thread.
class AbpNetworkCapture {
 public:
  // Maximum number of requests to keep in memory.
  static constexpr size_t kMaxRequests = 1000;

  AbpNetworkCapture();
  ~AbpNetworkCapture();

  AbpNetworkCapture(const AbpNetworkCapture&) = delete;
  AbpNetworkCapture& operator=(const AbpNetworkCapture&) = delete;

  // --- Configuration ---

  // Returns true if capture is currently enabled.
  bool IsEnabled() const { return enabled_; }

  // Returns the active config (meaningful only when IsEnabled()).
  const abp::NetworkConfig& config() const { return config_; }

  // Enable capture with the given config.
  // Does NOT send Network.enable — the caller is responsible for enabling the
  // CDP Network domain on the tab's AbpCdpClient before routing events here.
  void EnableCapture(const abp::NetworkConfig& config);

  // Disable capture and clear all buffered requests.
  void DisableCapture();

  // --- Event ingestion ---

  // Feed a CDP Network.* event into the capture buffer.
  // |method| is the CDP method name (e.g. "Network.requestWillBeSent").
  // |params| is the parsed CDP params dict.
  // |current_action_id| is associated with this event (may be empty).
  void OnCdpNetworkEvent(const std::string& method,
                         const base::Value::Dict& params,
                         const std::string& current_action_id);

  // --- Data access ---

  // Returns all captured requests in insertion order.
  const std::vector<abp::CapturedRequest>& GetRequests() const {
    return requests_;
  }

  // Find a request by CDP request_id. Returns nullptr if not found.
  abp::CapturedRequest* FindRequest(const std::string& request_id);

  // Clear all captured requests and the index.
  void Clear();

 private:
  // CDP event handlers
  void OnRequestWillBeSent(const base::Value::Dict& params,
                           const std::string& action_id);
  void OnResponseReceived(const base::Value::Dict& params);
  void OnLoadingFinished(const base::Value::Dict& params);
  void OnLoadingFailed(const base::Value::Dict& params);
  void OnRequestWillBeSentExtraInfo(const base::Value::Dict& params);
  void OnResponseReceivedExtraInfo(const base::Value::Dict& params);

  // Returns true if |resource_type| passes the config filter.
  bool PassesTypeFilter(const std::string& resource_type) const;

  // If |requests_| is at kMaxRequests, remove the oldest entry and
  // rebuild the index.
  void EvictOldestIfFull();

  bool enabled_ = false;
  abp::NetworkConfig config_;

  // Requests in insertion order.
  std::vector<abp::CapturedRequest> requests_;

  // Index: CDP request_id → index into requests_.
  std::unordered_map<std::string, size_t> request_index_;

  base::WeakPtrFactory<AbpNetworkCapture> weak_factory_{this};
};

#endif  // CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_
