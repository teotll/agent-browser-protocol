// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_
#define CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"

// Forward declarations.
namespace abp {
class AbpCdpClient;
}  // namespace abp

// Per-tab in-memory buffer that listens to CDP Network domain events and
// assembles abp::CapturedRequest records.
//
// Usage:
//   1. Call SetCaptureTypes() to configure which resource types to capture.
//   2. Call SetCurrentActionId() before each action to tag captured requests.
//   3. Call OnNetworkEvent() for every CDP Network.* event received.
//   4. Call GetRequests() / FindRequest() to read captured data.
//   5. Call ClearBuffer() to discard all captured requests.
//   6. Call MarkCorsPreflight() to flag a request as a CORS preflight.
//
// Must be created and used on the UI thread.
class AbpNetworkCapture {
 public:
  // Maximum number of requests to keep in memory.
  static constexpr size_t kMaxBufferSize = 1000;

  AbpNetworkCapture();
  ~AbpNetworkCapture();

  AbpNetworkCapture(const AbpNetworkCapture&) = delete;
  AbpNetworkCapture& operator=(const AbpNetworkCapture&) = delete;

  // --- Configuration ---

  // Set the resource types to capture (e.g. {"XHR", "Fetch"}).
  // Empty set means capture all types.
  void SetCaptureTypes(const std::set<std::string>& types);

  // Set the action ID to associate with subsequently captured requests.
  void SetCurrentActionId(const std::string& action_id);

  // --- Event ingestion ---

  // Feed a CDP Network.* event into the capture buffer.
  // |method| is the CDP method name (e.g. "Network.requestWillBeSent").
  // |params| is the parsed CDP params dict.
  // |cdp_client| is used for eager body fetching on loadingFinished.
  void OnNetworkEvent(const std::string& method,
                      const base::Value::Dict& params,
                      abp::AbpCdpClient* cdp_client);

  // --- Data access ---

  // Returns all captured requests in insertion order.
  const std::vector<abp::CapturedRequest>& GetRequests() const {
    return requests_;
  }

  // Query captured requests with filters, returning results as a JSON list.
  // Applies the same filtering logic as the network database query.
  // |tab_id| is stamped onto each result since the buffer doesn't store it.
  base::Value::List QueryBuffer(
      const abp::NetworkQueryFilter& filter,
      const std::string& tab_id) const;

  // Find a request by CDP request_id. Returns nullptr if not found.
  abp::CapturedRequest* FindRequest(const std::string& request_id);

  // Returns the total number of captured requests.
  int GetTotalCount() const;

  // Returns the number of completed requests.
  int GetCompletedCount() const;

  // Returns the number of pending (not yet completed) requests.
  int GetPendingCount() const;

  // Clear all captured requests and the index.
  void ClearBuffer();

  // Mark a request as a CORS preflight by request_id.
  // Sets cors_preflight=true on the entry if found.
  void MarkCorsPreflight(const std::string& request_id);

 private:
  // CDP event handlers
  void OnRequestWillBeSent(const base::Value::Dict& params);
  void OnResponseReceived(const base::Value::Dict& params);
  void OnLoadingFinished(const base::Value::Dict& params,
                         abp::AbpCdpClient* cdp_client);
  void OnLoadingFailed(const base::Value::Dict& params);
  void OnRequestWillBeSentExtraInfo(const base::Value::Dict& params);
  void OnResponseReceivedExtraInfo(const base::Value::Dict& params);
  void OnRequestServedFromServiceWorker(const base::Value::Dict& params);

  // Called when Network.getResponseBody response arrives.
  void OnResponseBodyReceived(const std::string& request_id,
                               bool success,
                               const std::string& response);

  // Returns true if |resource_type| passes the capture_types_ filter.
  bool PassesTypeFilter(const std::string& resource_type) const;

  // Remove entry at |index| from requests_ using the swap-with-last trick.
  // Updates request_index_ accordingly.
  void RemoveAtIndex(size_t index);

  // If |requests_| is at kMaxBufferSize, remove the oldest entry.
  void EvictOldestIfFull();

  // Resource types to capture. Empty = capture all.
  std::set<std::string> capture_types_;

  // Action ID to associate with new requests.
  std::string current_action_id_;

  // Requests in insertion order.
  std::vector<abp::CapturedRequest> requests_;

  // Index: CDP request_id → index into requests_.
  std::unordered_map<std::string, size_t> request_index_;

  // Track CORS preflight request IDs (OPTIONS requests).
  std::set<std::string> cors_preflight_ids_;

  // Track service-worker-served request IDs.
  std::set<std::string> service_worker_request_ids_;

  base::WeakPtrFactory<AbpNetworkCapture> weak_factory_{this};
};

#endif  // CHROME_BROWSER_ABP_ABP_NETWORK_CAPTURE_H_
