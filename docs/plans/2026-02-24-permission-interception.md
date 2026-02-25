# Permission Request Interception Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Intercept all permission prompts in ABP, auto-deny non-geolocation, surface geolocation as pending events with agent grant/deny via full ABP action lifecycle, and provide undetectable mock geolocation via custom LocationProvider.

**Architecture:** PermissionRequestManager observer per tab detects permission prompts — geolocation stored as pending, others auto-denied. Agent responds via REST/MCP, triggering an ABP action (resume → grant → wait → pause → screenshot). Mock geolocation provided via `ContentBrowserClient::OverrideSystemLocationProvider()` — no CDP, no detection surface.

**Tech Stack:** C++ (Chromium), PermissionRequestManager Observer, device::LocationProvider, AbpActionContext

---

### Task 1: Create AbpLocationProvider

**Files:**
- Create: `chrome/browser/abp/abp_location_provider.h`
- Create: `chrome/browser/abp/abp_location_provider.cc`

**Step 1: Write the header**

```cpp
// chrome/browser/abp/abp_location_provider.h
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_LOCATION_PROVIDER_H_
#define CHROME_BROWSER_ABP_ABP_LOCATION_PROVIDER_H_

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "services/device/public/cpp/geolocation/location_provider.h"
#include "services/device/public/mojom/geolocation_internals.mojom.h"
#include "services/device/public/mojom/geoposition.mojom.h"

namespace abp {

// Custom LocationProvider that serves mock geolocation coordinates.
// When no mock is set, returns kPositionUnavailable.
// Modeled on device::FakeLocationProvider.
class AbpLocationProvider : public device::LocationProvider {
 public:
  AbpLocationProvider();
  ~AbpLocationProvider() override;

  AbpLocationProvider(const AbpLocationProvider&) = delete;
  AbpLocationProvider& operator=(const AbpLocationProvider&) = delete;

  // Set mock position. Notifies all listeners immediately.
  void SetPosition(double latitude, double longitude, double accuracy);

  // Clear mock position. Reverts to kPositionUnavailable.
  void ClearPosition();

  // Whether mock coordinates are currently set.
  bool has_position() const { return has_position_; }

  // device::LocationProvider:
  void FillDiagnostics(
      device::mojom::GeolocationDiagnostics& diagnostics) override;
  void SetUpdateCallback(
      const LocationProviderUpdateCallback& callback) override;
  void StartProvider(bool high_accuracy) override;
  void StopProvider() override;
  const device::mojom::GeopositionResult* GetPosition() override;
  void OnPermissionGranted() override;

  // Get the singleton instance (created on first call).
  // Thread-safe: always returns same pointer.
  static AbpLocationProvider* GetInstance();

 private:
  void NotifyListeners();

  device::mojom::GeolocationDiagnostics::ProviderState state_ =
      device::mojom::GeolocationDiagnostics::ProviderState::kStopped;
  bool is_permission_granted_ = false;
  bool has_position_ = false;
  device::mojom::GeopositionResultPtr result_;
  LocationProviderUpdateCallback callback_;
  scoped_refptr<base::SingleThreadTaskRunner> provider_task_runner_;

  base::WeakPtrFactory<AbpLocationProvider> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_LOCATION_PROVIDER_H_
```

**Step 2: Write the implementation**

```cpp
// chrome/browser/abp/abp_location_provider.cc
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_location_provider.h"

#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"

namespace abp {

namespace {
AbpLocationProvider* g_instance = nullptr;
}  // namespace

AbpLocationProvider::AbpLocationProvider()
    : provider_task_runner_(
          base::SingleThreadTaskRunner::GetCurrentDefault()) {
  // Initialize with "position unavailable" error
  result_ = device::mojom::GeopositionResult::NewError(
      device::mojom::GeopositionError::New(
          device::mojom::GeopositionErrorCode::kPositionUnavailable,
          /*error_message=*/"ABP: No mock location set",
          /*error_technical=*/""));
}

AbpLocationProvider::~AbpLocationProvider() {
  if (g_instance == this)
    g_instance = nullptr;
}

// static
AbpLocationProvider* AbpLocationProvider::GetInstance() {
  return g_instance;
}

void AbpLocationProvider::SetPosition(double latitude,
                                      double longitude,
                                      double accuracy) {
  has_position_ = true;
  auto position = device::mojom::Geoposition::New();
  position->latitude = latitude;
  position->longitude = longitude;
  position->accuracy = accuracy;
  position->altitude = 0.0;
  position->altitude_accuracy = -1.0;
  position->heading = -1.0;
  position->speed = -1.0;
  position->timestamp = base::Time::Now();
  result_ = device::mojom::GeopositionResult::NewPosition(std::move(position));
  VLOG(1) << "ABP: Mock geolocation set: " << latitude << ", " << longitude
          << " accuracy=" << accuracy;
  NotifyListeners();
}

void AbpLocationProvider::ClearPosition() {
  has_position_ = false;
  result_ = device::mojom::GeopositionResult::NewError(
      device::mojom::GeopositionError::New(
          device::mojom::GeopositionErrorCode::kPositionUnavailable,
          /*error_message=*/"ABP: No mock location set",
          /*error_technical=*/""));
  VLOG(1) << "ABP: Mock geolocation cleared";
  NotifyListeners();
}

void AbpLocationProvider::NotifyListeners() {
  if (provider_task_runner_->BelongsToCurrentThread()) {
    if (!callback_.is_null())
      callback_.Run(this, result_.Clone());
  } else {
    provider_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&AbpLocationProvider::NotifyListeners,
                        weak_factory_.GetWeakPtr()));
  }
}

void AbpLocationProvider::FillDiagnostics(
    device::mojom::GeolocationDiagnostics& diagnostics) {
  diagnostics.provider_state = state_;
}

void AbpLocationProvider::SetUpdateCallback(
    const LocationProviderUpdateCallback& callback) {
  callback_ = callback;
}

void AbpLocationProvider::StartProvider(bool high_accuracy) {
  state_ = high_accuracy
               ? device::mojom::GeolocationDiagnostics::ProviderState::kHighAccuracy
               : device::mojom::GeolocationDiagnostics::ProviderState::kLowAccuracy;
  // Immediately notify with current position (mock or error)
  if (!callback_.is_null())
    callback_.Run(this, result_.Clone());
}

void AbpLocationProvider::StopProvider() {
  state_ = device::mojom::GeolocationDiagnostics::ProviderState::kStopped;
}

const device::mojom::GeopositionResult* AbpLocationProvider::GetPosition() {
  return result_.get();
}

void AbpLocationProvider::OnPermissionGranted() {
  is_permission_granted_ = true;
}

}  // namespace abp
```

**Step 3: Add to BUILD.gn**

Add to `chrome/browser/abp/BUILD.gn` in the `source_set("abp")` sources list (alphabetically):

```gn
    "abp_location_provider.cc",
    "abp_location_provider.h",
```

Add to deps:

```gn
    "//services/device/public/cpp/geolocation",
    "//services/device/public/mojom",
```

**Step 4: Verify build compiles**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: BUILD SUCCESS

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_location_provider.h \
        chrome/browser/abp/abp_location_provider.cc \
        chrome/browser/abp/BUILD.gn
git commit -m "feat(abp): add AbpLocationProvider for mock geolocation"
```

---

### Task 2: Hook AbpLocationProvider into ContentBrowserClient

**Files:**
- Modify: `chrome/browser/chrome_content_browser_client.h` (~line 473)
- Modify: `chrome/browser/chrome_content_browser_client.cc`

**Step 1: Add override declaration to header**

In `chrome/browser/chrome_content_browser_client.h`, after `GetGeolocationApiKey()` (line 472):

```cpp
  std::unique_ptr<device::LocationProvider>
  OverrideSystemLocationProvider() override;
```

**Step 2: Add implementation**

In `chrome/browser/chrome_content_browser_client.cc`, add include:

```cpp
#include "chrome/browser/abp/abp_location_provider.h"
#include "chrome/browser/abp/abp_switches.h"
```

Add implementation near the existing `GetGeolocationApiKey()` method:

```cpp
std::unique_ptr<device::LocationProvider>
ChromeContentBrowserClient::OverrideSystemLocationProvider() {
  // Only override when ABP is active (port flag present or default)
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch("test-type")) {
    auto provider = std::make_unique<abp::AbpLocationProvider>();
    // Store singleton reference for ABP controller access.
    // The provider is owned by the geolocation system; we just keep a
    // raw pointer for setting coordinates via the ABP API.
    // Note: g_instance is set here since the provider is created on the
    // geolocation thread, but GetInstance() is called from UI thread.
    // This is safe because the provider outlives all ABP API calls.
    return provider;
  }
  return nullptr;
}
```

Note: The singleton `g_instance` needs to be set when the provider is created. Update `AbpLocationProvider` constructor:

In `abp_location_provider.cc` constructor, add:
```cpp
  g_instance = this;
```

**Step 3: Add BUILD.gn dep**

In `chrome/browser/BUILD.gn` (or wherever `chrome_content_browser_client.cc` is built), ensure `//chrome/browser/abp` is already a dependency. Check:

Run: `grep -r "chrome/browser/abp" chrome/browser/BUILD.gn | head -5`

If not present, this dep flows through the existing abp inclusion in `chrome_browser_main.cc`.

**Step 4: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: BUILD SUCCESS

**Step 5: Commit**

```bash
git add chrome/browser/chrome_content_browser_client.h \
        chrome/browser/chrome_content_browser_client.cc \
        chrome/browser/abp/abp_location_provider.cc
git commit -m "feat(abp): hook AbpLocationProvider into ContentBrowserClient"
```

---

### Task 3: Create AbpPermissionObserver

**Files:**
- Create: `chrome/browser/abp/abp_permission_observer.h`
- Create: `chrome/browser/abp/abp_permission_observer.cc`

**Step 1: Write the header**

```cpp
// chrome/browser/abp/abp_permission_observer.h
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_PERMISSION_OBSERVER_H_
#define CHROME_BROWSER_ABP_ABP_PERMISSION_OBSERVER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "components/permissions/permission_request_manager.h"

namespace abp {

class AbpController;

// Observes PermissionRequestManager to intercept permission prompts.
// Geolocation: stored as pending, agent decides via API.
// All others: auto-denied, event emitted.
class AbpPermissionObserver
    : public permissions::PermissionRequestManager::Observer {
 public:
  explicit AbpPermissionObserver(AbpController* controller);
  ~AbpPermissionObserver() override;

  AbpPermissionObserver(const AbpPermissionObserver&) = delete;
  AbpPermissionObserver& operator=(const AbpPermissionObserver&) = delete;

  // Attach to a tab's PermissionRequestManager. Call on tab creation.
  void AttachToTab(const std::string& tab_id,
                   content::WebContents* web_contents);

  // Detach from a tab. Call on tab close.
  void DetachFromTab(const std::string& tab_id);

  // Grant the pending geolocation permission for a tab.
  // Returns false if no pending permission for this tab.
  bool GrantPermission(const std::string& tab_id);

  // Deny the pending geolocation permission for a tab.
  // Returns false if no pending permission for this tab.
  bool DenyPermission(const std::string& tab_id);

  // Get the pending permission request ID for a tab, or empty string.
  std::string GetPendingPermissionId(const std::string& tab_id) const;

 private:
  // PermissionRequestManager::Observer:
  void OnPromptAdded() override;
  void OnPromptRemoved() override;
  void OnPermissionRequestManagerDestructed() override;

  std::string GeneratePermissionId();

  // Find tab_id for the manager that triggered the current callback.
  std::string FindTabIdForManager(
      permissions::PermissionRequestManager* manager) const;

  raw_ptr<AbpController> controller_;

  // Per-tab observation tracking
  struct TabObservation {
    raw_ptr<permissions::PermissionRequestManager> manager = nullptr;
    base::ScopedObservation<permissions::PermissionRequestManager,
                            permissions::PermissionRequestManager::Observer>
        observation;
    std::string pending_permission_id;

    TabObservation();
    ~TabObservation();
    TabObservation(TabObservation&&);
    TabObservation& operator=(TabObservation&&);
  };

  std::map<std::string, TabObservation> tab_observations_;  // tab_id -> obs

  int next_permission_id_ = 1;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_PERMISSION_OBSERVER_H_
```

**Step 2: Write the implementation**

```cpp
// chrome/browser/abp/abp_permission_observer.cc
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_permission_observer.h"

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/abp/abp_controller.h"
#include "components/permissions/permission_request.h"
#include "components/permissions/request_type.h"
#include "content/public/browser/web_contents.h"

namespace abp {

AbpPermissionObserver::TabObservation::TabObservation() = default;
AbpPermissionObserver::TabObservation::~TabObservation() = default;
AbpPermissionObserver::TabObservation::TabObservation(TabObservation&&) =
    default;
AbpPermissionObserver::TabObservation&
AbpPermissionObserver::TabObservation::operator=(TabObservation&&) = default;

AbpPermissionObserver::AbpPermissionObserver(AbpController* controller)
    : controller_(controller) {}

AbpPermissionObserver::~AbpPermissionObserver() {
  // Detach from all managers
  tab_observations_.clear();
}

std::string AbpPermissionObserver::GeneratePermissionId() {
  return base::StringPrintf("perm_%d", next_permission_id_++);
}

void AbpPermissionObserver::AttachToTab(const std::string& tab_id,
                                        content::WebContents* web_contents) {
  if (!web_contents)
    return;

  auto* manager =
      permissions::PermissionRequestManager::FromWebContents(web_contents);
  if (!manager)
    return;

  // Don't double-attach
  if (tab_observations_.count(tab_id))
    return;

  TabObservation obs;
  obs.manager = manager;
  obs.observation.Observe(manager);
  tab_observations_[tab_id] = std::move(obs);

  VLOG(1) << "ABP: Permission observer attached to tab " << tab_id;
}

void AbpPermissionObserver::DetachFromTab(const std::string& tab_id) {
  tab_observations_.erase(tab_id);
  VLOG(1) << "ABP: Permission observer detached from tab " << tab_id;
}

std::string AbpPermissionObserver::FindTabIdForManager(
    permissions::PermissionRequestManager* manager) const {
  for (const auto& [tab_id, obs] : tab_observations_) {
    if (obs.manager == manager)
      return tab_id;
  }
  return std::string();
}

void AbpPermissionObserver::OnPromptAdded() {
  // Find which tab this prompt belongs to by checking all observed managers
  for (auto& [tab_id, obs] : tab_observations_) {
    auto* manager = obs.manager.get();
    if (!manager)
      continue;

    const auto& requests = manager->Requests();
    if (requests.empty())
      continue;

    // Check if any request is geolocation
    bool has_geolocation = false;
    std::string origin;
    std::string permission_type;

    for (const auto& request : requests) {
      origin = request->requesting_origin().spec();

      if (request->request_type() ==
          permissions::RequestType::kGeolocation) {
        has_geolocation = true;
        permission_type = "geolocation";
        break;
      }
    }

    if (has_geolocation) {
      // Store as pending — agent decides
      std::string perm_id = GeneratePermissionId();
      obs.pending_permission_id = perm_id;

      VLOG(1) << "ABP: Geolocation permission requested in tab " << tab_id
              << " origin=" << origin << " id=" << perm_id;

      // Emit event for action context
      base::Value::Dict event_data;
      event_data.Set("id", perm_id);
      event_data.Set("tab_id", tab_id);
      event_data.Set("permission_type", permission_type);
      event_data.Set("origin", origin);
      controller_->EmitPopupEvent("permission_requested",
                                  std::move(event_data));

      // Notify controller of pending permission
      controller_->OnPermissionRequested(perm_id, tab_id, permission_type,
                                         origin);
    } else {
      // Auto-deny all non-geolocation permissions
      for (const auto& request : requests) {
        std::string type_str = "unknown";
        // Map common types to strings
        switch (request->request_type()) {
          case permissions::RequestType::kNotifications:
            type_str = "notifications";
            break;
          case permissions::RequestType::kCameraStream:
            type_str = "camera";
            break;
          case permissions::RequestType::kMicStream:
            type_str = "microphone";
            break;
          default:
            type_str = "other";
            break;
        }

        base::Value::Dict event_data;
        event_data.Set("permission_type", type_str);
        event_data.Set("origin", request->requesting_origin().spec());
        event_data.Set("tab_id", tab_id);
        controller_->EmitPopupEvent("permission_auto_denied",
                                    std::move(event_data));
      }

      VLOG(1) << "ABP: Auto-denying non-geolocation permission in tab "
              << tab_id;
      manager->Deny();
    }
  }
}

void AbpPermissionObserver::OnPromptRemoved() {
  // Prompt was dismissed externally (e.g., navigation). Clear pending state.
  for (auto& [tab_id, obs] : tab_observations_) {
    if (!obs.pending_permission_id.empty()) {
      controller_->OnPermissionDismissed(obs.pending_permission_id, tab_id);
      obs.pending_permission_id.clear();
    }
  }
}

void AbpPermissionObserver::OnPermissionRequestManagerDestructed() {
  // Find and remove the destroyed manager's observation
  for (auto it = tab_observations_.begin(); it != tab_observations_.end();) {
    // ScopedObservation handles the actual unsubscribe.
    // We just need to identify which one was destructed.
    // Since we can't tell which manager triggered this, check if any
    // manager pointer is now invalid by checking observation state.
    ++it;
  }
}

bool AbpPermissionObserver::GrantPermission(const std::string& tab_id) {
  auto it = tab_observations_.find(tab_id);
  if (it == tab_observations_.end())
    return false;

  auto* manager = it->second.manager.get();
  if (!manager || it->second.pending_permission_id.empty())
    return false;

  VLOG(1) << "ABP: Granting permission in tab " << tab_id
          << " id=" << it->second.pending_permission_id;

  it->second.pending_permission_id.clear();
  manager->Accept();
  return true;
}

bool AbpPermissionObserver::DenyPermission(const std::string& tab_id) {
  auto it = tab_observations_.find(tab_id);
  if (it == tab_observations_.end())
    return false;

  auto* manager = it->second.manager.get();
  if (!manager || it->second.pending_permission_id.empty())
    return false;

  VLOG(1) << "ABP: Denying permission in tab " << tab_id
          << " id=" << it->second.pending_permission_id;

  it->second.pending_permission_id.clear();
  manager->Deny();
  return true;
}

std::string AbpPermissionObserver::GetPendingPermissionId(
    const std::string& tab_id) const {
  auto it = tab_observations_.find(tab_id);
  if (it == tab_observations_.end())
    return std::string();
  return it->second.pending_permission_id;
}

}  // namespace abp
```

**Step 3: Add to BUILD.gn**

Add to `chrome/browser/abp/BUILD.gn` sources (alphabetically):

```gn
    "abp_permission_observer.cc",
    "abp_permission_observer.h",
```

Add to deps:

```gn
    "//components/permissions",
```

**Step 4: Verify build compiles**

Run: `autoninja -C out/Default chrome/browser/abp:abp`
Expected: BUILD SUCCESS

**Step 5: Commit**

```bash
git add chrome/browser/abp/abp_permission_observer.h \
        chrome/browser/abp/abp_permission_observer.cc \
        chrome/browser/abp/BUILD.gn
git commit -m "feat(abp): add AbpPermissionObserver for permission interception"
```

---

### Task 4: Wire AbpPermissionObserver into AbpController

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h`
- Modify: `chrome/browser/abp/abp_controller.cc`

**Step 1: Add includes and forward declaration to header**

In `abp_controller.h`, add include:

```cpp
#include "chrome/browser/abp/abp_permission_observer.h"
```

**Step 2: Add pending permission state to controller**

In `abp_controller.h`, add struct (near `PendingDialog`):

```cpp
struct PendingPermissionRequest {
  std::string id;               // "perm_1", "perm_2", etc.
  std::string tab_id;
  std::string permission_type;  // "geolocation"
  std::string origin;
  int64_t requested_at_ms = 0;
};
```

Add to AbpController private members:

```cpp
  std::unique_ptr<AbpPermissionObserver> permission_observer_;
  std::map<std::string, PendingPermissionRequest> pending_permissions_;
```

**Step 3: Add public methods to header**

```cpp
  // Permission handling
  void OnPermissionRequested(const std::string& perm_id,
                             const std::string& tab_id,
                             const std::string& permission_type,
                             const std::string& origin);
  void OnPermissionDismissed(const std::string& perm_id,
                             const std::string& tab_id);
  void ListPendingPermissions(ResponseCallback callback);
  void GrantPermission(const std::string& perm_id,
                       const base::Value::Dict& params,
                       ResponseCallback callback);
  void DenyPermission(const std::string& perm_id,
                      const base::Value::Dict& params,
                      ResponseCallback callback);

  // Geolocation mock
  void SetGeolocation(const base::Value::Dict& params,
                      ResponseCallback callback);
  void ClearGeolocation(ResponseCallback callback);

  // Accessor for permission observer
  AbpPermissionObserver* permission_observer() {
    return permission_observer_.get();
  }
```

**Step 4: Initialize observer in constructor**

In `abp_controller.cc`, where `popup_interceptor_` is created, add:

```cpp
  permission_observer_ = std::make_unique<AbpPermissionObserver>(this);
```

**Step 5: Attach observer on tab creation**

Find where `popup_interceptor_->SetPopupInterceptor()` is called for new tabs. Add alongside it:

```cpp
  if (permission_observer_) {
    permission_observer_->AttachToTab(tab_id, wc);
  }
```

Do this in all three registration points (tab creation, FindWebContents lazy attach, DeterministicPauseAllTabs).

**Step 6: Detach observer on tab close**

Find where `popup_interceptor_->CleanupForTab(tab_id)` is called. Add alongside it:

```cpp
  if (permission_observer_) {
    permission_observer_->DetachFromTab(tab_id);
  }
  // Also clean pending permissions for this tab
  for (auto it = pending_permissions_.begin();
       it != pending_permissions_.end();) {
    if (it->second.tab_id == tab_id) {
      it = pending_permissions_.erase(it);
    } else {
      ++it;
    }
  }
```

**Step 7: Implement OnPermissionRequested and OnPermissionDismissed**

```cpp
void AbpController::OnPermissionRequested(const std::string& perm_id,
                                          const std::string& tab_id,
                                          const std::string& permission_type,
                                          const std::string& origin) {
  PendingPermissionRequest req;
  req.id = perm_id;
  req.tab_id = tab_id;
  req.permission_type = permission_type;
  req.origin = origin;
  req.requested_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  pending_permissions_[perm_id] = std::move(req);
}

void AbpController::OnPermissionDismissed(const std::string& perm_id,
                                          const std::string& tab_id) {
  pending_permissions_.erase(perm_id);
}
```

**Step 8: Implement ListPendingPermissions**

```cpp
void AbpController::ListPendingPermissions(ResponseCallback callback) {
  base::Value::List list;
  for (const auto& [id, req] : pending_permissions_) {
    base::Value::Dict d;
    d.Set("id", req.id);
    d.Set("tab_id", req.tab_id);
    d.Set("permission_type", req.permission_type);
    d.Set("origin", req.origin);
    d.Set("requested_at", req.requested_at_ms);
    list.Append(std::move(d));
  }
  base::Value::Dict response;
  response.Set("permissions", std::move(list));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}
```

**Step 9: Implement GrantPermission as ABP action**

```cpp
void AbpController::GrantPermission(const std::string& perm_id,
                                    const base::Value::Dict& params,
                                    ResponseCallback callback) {
  auto it = pending_permissions_.find(perm_id);
  if (it == pending_permissions_.end()) {
    SendError(404, "No pending permission with id: " + perm_id,
              std::move(callback));
    return;
  }

  std::string tab_id = it->second.tab_id;
  std::string permission_type = it->second.permission_type;

  // Remove from pending before action (observer clears its state in Grant)
  pending_permissions_.erase(it);

  AbpActionContext::Run(
      this, tab_id, "permission_grant", params,
      base::BindOnce(
          [](std::string perm_type, AbpActionContext* ctx) {
            auto* controller = ctx->controller();
            if (!controller->permission_observer()->GrantPermission(
                    ctx->tab_id())) {
              ctx->OnActionError("PERMISSION_ERROR",
                                 "Failed to grant " + perm_type +
                                     " permission — prompt may have been "
                                     "dismissed");
              return;
            }
            base::Value::Dict res;
            res.Set("status", "granted");
            res.Set("permission_type", perm_type);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(permission_type)),
      std::move(callback));
}
```

**Step 10: Implement DenyPermission as ABP action**

```cpp
void AbpController::DenyPermission(const std::string& perm_id,
                                   const base::Value::Dict& params,
                                   ResponseCallback callback) {
  auto it = pending_permissions_.find(perm_id);
  if (it == pending_permissions_.end()) {
    SendError(404, "No pending permission with id: " + perm_id,
              std::move(callback));
    return;
  }

  std::string tab_id = it->second.tab_id;
  std::string permission_type = it->second.permission_type;

  pending_permissions_.erase(it);

  AbpActionContext::Run(
      this, tab_id, "permission_deny", params,
      base::BindOnce(
          [](std::string perm_type, AbpActionContext* ctx) {
            auto* controller = ctx->controller();
            if (!controller->permission_observer()->DenyPermission(
                    ctx->tab_id())) {
              ctx->OnActionError("PERMISSION_ERROR",
                                 "Failed to deny " + perm_type +
                                     " permission — prompt may have been "
                                     "dismissed");
              return;
            }
            base::Value::Dict res;
            res.Set("status", "denied");
            res.Set("permission_type", perm_type);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(permission_type)),
      std::move(callback));
}
```

**Step 11: Implement SetGeolocation and ClearGeolocation (non-actions)**

```cpp
void AbpController::SetGeolocation(const base::Value::Dict& params,
                                   ResponseCallback callback) {
  auto lat = params.FindDouble("latitude");
  auto lng = params.FindDouble("longitude");
  if (!lat || !lng) {
    SendError(400, "Missing 'latitude' or 'longitude'", std::move(callback));
    return;
  }
  double accuracy = params.FindDouble("accuracy").value_or(100.0);

  auto* provider = abp::AbpLocationProvider::GetInstance();
  if (!provider) {
    SendError(500, "Location provider not available", std::move(callback));
    return;
  }

  provider->SetPosition(*lat, *lng, accuracy);

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("latitude", *lat);
  response.Set("longitude", *lng);
  response.Set("accuracy", accuracy);
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::ClearGeolocation(ResponseCallback callback) {
  auto* provider = abp::AbpLocationProvider::GetInstance();
  if (!provider) {
    SendError(500, "Location provider not available", std::move(callback));
    return;
  }

  provider->ClearPosition();

  base::Value::Dict response;
  response.Set("success", true);
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}
```

**Step 12: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: BUILD SUCCESS

**Step 13: Commit**

```bash
git add chrome/browser/abp/abp_controller.h \
        chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): wire permission observer and geolocation into controller"
```

---

### Task 5: Add REST API Routes

**Files:**
- Modify: `chrome/browser/abp/abp_controller.cc` (HandleRequest method)

**Step 1: Add permission routes**

In the `HandleRequest()` method, after the `select` resource block (~line 2113), add:

```cpp
  // Route: /api/v1/permissions
  if (resource == "permissions") {
    if (segments.size() == 3) {
      // GET /api/v1/permissions
      if (method == "GET") {
        ListPendingPermissions(std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 5) {
      const std::string& perm_id = segments[3];
      const std::string& action = segments[4];
      if (method == "POST") {
        if (action == "grant") {
          GrantPermission(perm_id, params, std::move(callback));
        } else if (action == "deny") {
          DenyPermission(perm_id, params, std::move(callback));
        } else {
          SendError(404, "Unknown permission action: " + action,
                    std::move(callback));
        }
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    SendError(404, "Not found", std::move(callback));
    return;
  }

  // Route: /api/v1/geolocation
  if (resource == "geolocation") {
    if (segments.size() == 3) {
      if (method == "POST") {
        SetGeolocation(params, std::move(callback));
      } else if (method == "DELETE") {
        ClearGeolocation(std::move(callback));
      } else if (method == "GET") {
        // Return current mock state
        auto* provider = abp::AbpLocationProvider::GetInstance();
        base::Value::Dict response;
        if (provider && provider->has_position()) {
          auto* pos = provider->GetPosition();
          if (pos && pos->is_position()) {
            response.Set("active", true);
            response.Set("latitude", pos->get_position()->latitude);
            response.Set("longitude", pos->get_position()->longitude);
            response.Set("accuracy", pos->get_position()->accuracy);
          } else {
            response.Set("active", false);
          }
        } else {
          response.Set("active", false);
        }
        SendJson(200, base::Value(std::move(response)), std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    SendError(404, "Not found", std::move(callback));
    return;
  }
```

**Step 2: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: BUILD SUCCESS

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): add REST routes for permissions and geolocation"
```

---

### Task 6: Add MCP Tools

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.h`
- Modify: `chrome/browser/abp/abp_mcp_handler.cc`

**Step 1: Add tool definitions in GetToolDefinitions()**

Add after the last existing tool definition:

```cpp
  // respond_to_permission
  tools.Append(
      ToolBuilder("respond_to_permission")
          .Description(
              "Respond to a pending browser permission prompt. When a page "
              "requests a permission (e.g. geolocation), a "
              "'permission_requested' event is emitted with an ID. Use this "
              "tool to grant or deny that permission. IMPORTANT: If granting "
              "geolocation permission, call set_geolocation FIRST to provide "
              "mock coordinates, then call this tool to grant.")
          .RequiredString("permission_id",
                          "The permission request ID from the "
                          "permission_requested event")
          .RequiredBoolean("allow", "True to grant, false to deny")
          .Build());

  // set_geolocation
  tools.Append(
      ToolBuilder("set_geolocation")
          .Description(
              "Set mock geolocation coordinates for the browser. This is NOT "
              "an action — it immediately sets the coordinates without "
              "affecting page execution. Call this BEFORE granting a "
              "geolocation permission so the page receives the correct "
              "coordinates when the permission is granted. Use action='clear' "
              "to remove the mock and revert to 'position unavailable'.")
          .OptionalString("action",
                          "Action to perform: 'set' (default) or 'clear'")
          .OptionalNumber("latitude",
                          "Latitude in degrees (-90 to 90). Required for "
                          "action='set'.")
          .OptionalNumber("longitude",
                          "Longitude in degrees (-180 to 180). Required for "
                          "action='set'.")
          .OptionalNumber("accuracy",
                          "Accuracy radius in meters (default: 100)")
          .Build());
```

**Step 2: Add dispatch handlers in HandleToolsCall()**

Add new branches:

```cpp
  } else if (*name == "respond_to_permission") {
    CallRespondToPermission(*args, std::move(request_id), std::move(callback));
  } else if (*name == "set_geolocation") {
    CallSetGeolocation(*args, std::move(request_id), std::move(callback));
  }
```

**Step 3: Add method declarations to header**

In `abp_mcp_handler.h`, add to private:

```cpp
  void CallRespondToPermission(const base::Value::Dict& args,
                               base::Value request_id,
                               ResponseWithHeadersCallback callback);
  void CallSetGeolocation(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
```

**Step 4: Implement CallRespondToPermission**

```cpp
void AbpMcpHandler::CallRespondToPermission(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* perm_id = args.FindString("permission_id");
  if (!perm_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing permission_id", std::move(callback));
    return;
  }

  auto allow = args.FindBool("allow");
  if (!allow.has_value()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing allow boolean", std::move(callback));
    return;
  }

  std::string action = *allow ? "grant" : "deny";
  std::string body = "{}";

  controller_->HandleRequest(
      "POST", "/api/v1/permissions/" + *perm_id + "/" + action, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 5: Implement CallSetGeolocation**

```cpp
void AbpMcpHandler::CallSetGeolocation(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "set";

  if (act == "clear") {
    controller_->HandleRequest(
        "DELETE", "/api/v1/geolocation", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
    return;
  }

  auto latitude = args.FindDouble("latitude");
  auto longitude = args.FindDouble("longitude");
  if (!latitude || !longitude) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing latitude or longitude", std::move(callback));
    return;
  }

  base::Value::Dict body_dict;
  body_dict.Set("latitude", *latitude);
  body_dict.Set("longitude", *longitude);
  if (auto accuracy = args.FindDouble("accuracy"))
    body_dict.Set("accuracy", *accuracy);

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/geolocation", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

**Step 6: Update MCP tool count**

Search for tool count assertions/comments (e.g., "14 tools") and update to 16.

**Step 7: Verify build compiles**

Run: `autoninja -C out/Default chrome`
Expected: BUILD SUCCESS

**Step 8: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h \
        chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): add MCP tools for permission response and geolocation"
```

---

### Task 7: Manual Testing

**Step 1: Build and launch ABP**

```bash
autoninja -C out/Default chrome
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

**Step 2: Test geolocation mock (non-action)**

```bash
# Set mock location (New York)
curl -X POST http://localhost:8222/api/v1/geolocation \
  -H "Content-Type: application/json" \
  -d '{"latitude": 40.7128, "longitude": -74.0060, "accuracy": 10}'

# Verify it's set
curl http://localhost:8222/api/v1/geolocation

# Clear it
curl -X DELETE http://localhost:8222/api/v1/geolocation
```

Expected: 200 with `{"success": true, ...}`

**Step 3: Test permission interception**

```bash
# Navigate to a page that requests geolocation
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url": "https://browserleaks.com/geo"}'

# Wait for page to load and request permission
# Check pending permissions
curl http://localhost:8222/api/v1/permissions

# Should see a pending geolocation permission
# Set mock location first
curl -X POST http://localhost:8222/api/v1/geolocation \
  -H "Content-Type: application/json" \
  -d '{"latitude": 40.7128, "longitude": -74.0060, "accuracy": 10}'

# Grant the permission (replace perm_1 with actual ID)
curl -X POST http://localhost:8222/api/v1/permissions/perm_1/grant \
  -H "Content-Type: application/json" \
  -d '{}'
```

Expected: Permission granted, page shows mock coordinates, screenshot captured.

**Step 4: Test auto-deny of notifications**

Navigate to a page that requests notification permission. Verify no prompt appears and `permission_auto_denied` event is emitted in action response.

**Step 5: Test MCP tools**

```bash
# Initialize MCP
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

# List tools — verify respond_to_permission and set_geolocation are present
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'

# Call set_geolocation
curl -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"set_geolocation","arguments":{"latitude":40.7128,"longitude":-74.006,"accuracy":10}}}'
```

**Step 6: Commit any fixes**

```bash
git add -A
git commit -m "fix(abp): address issues found during manual testing"
```

---

### Task 8: Update Documentation

**Files:**
- Modify: `CLAUDE.md` — add permission and geolocation endpoints to API table
- Modify: `plans/API.md` — add full endpoint documentation (if it exists and is maintained)

**Step 1: Update CLAUDE.md API table**

Add to the endpoint table:

```markdown
| **Permissions** | | |
| GET | `/api/v1/permissions` | List pending permission requests |
| POST | `/api/v1/permissions/{id}/grant` | Grant permission (ABP action) |
| POST | `/api/v1/permissions/{id}/deny` | Deny permission (ABP action) |
| **Geolocation** | | |
| GET | `/api/v1/geolocation` | Get mock geolocation state |
| POST | `/api/v1/geolocation` | Set mock coordinates |
| DELETE | `/api/v1/geolocation` | Clear mock coordinates |
```

**Step 2: Update MCP tool count**

Change "14 tools" to "16 tools" in CLAUDE.md where MCP is described.

**Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: add permission and geolocation endpoints to CLAUDE.md"
```
