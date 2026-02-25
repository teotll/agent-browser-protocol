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

// --- TabPermissionObserver ---

AbpPermissionObserver::TabPermissionObserver::TabPermissionObserver(
    AbpPermissionObserver* owner,
    const std::string& tab_id,
    permissions::PermissionRequestManager* manager)
    : owner_(owner), tab_id_(tab_id), manager_(manager) {
  manager_->AddObserver(this);
}

AbpPermissionObserver::TabPermissionObserver::~TabPermissionObserver() {
  if (manager_)
    manager_->RemoveObserver(this);
}

void AbpPermissionObserver::TabPermissionObserver::OnPromptAdded() {
  if (!manager_ || !owner_ || !owner_->controller_)
    return;

  const auto& requests = manager_->Requests();
  if (requests.empty())
    return;

  // Check if any request is geolocation
  bool has_geolocation = false;
  std::string origin;

  for (const auto& request : requests) {
    origin = request->requesting_origin().spec();
    if (request->request_type() == permissions::RequestType::kGeolocation) {
      has_geolocation = true;
      break;
    }
  }

  if (has_geolocation) {
    // Store as pending -- agent decides via API
    std::string perm_id = owner_->GeneratePermissionId();
    pending_permission_id_ = perm_id;

    VLOG(1) << "ABP: Geolocation permission requested in tab " << tab_id_
            << " origin=" << origin << " id=" << perm_id;

    // Emit event for action context
    base::Value::Dict event_data;
    event_data.Set("id", perm_id);
    event_data.Set("tab_id", tab_id_);
    event_data.Set("permission_type", "geolocation");
    event_data.Set("origin", origin);
    owner_->controller_->EmitPopupEvent("permission_requested",
                                        std::move(event_data));

    // Notify controller of pending permission
    owner_->controller_->OnPermissionRequested(perm_id, tab_id_,
                                               "geolocation", origin);
  } else {
    // Auto-deny all non-geolocation permissions
    for (const auto& request : requests) {
      std::string type_str;
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
      event_data.Set("tab_id", tab_id_);
      owner_->controller_->EmitPopupEvent("permission_auto_denied",
                                          std::move(event_data));
    }

    VLOG(1) << "ABP: Auto-denying non-geolocation permission in tab "
            << tab_id_;
    manager_->Deny();
  }
}

void AbpPermissionObserver::TabPermissionObserver::OnPromptRemoved() {
  if (!pending_permission_id_.empty() && owner_ && owner_->controller_) {
    owner_->controller_->OnPermissionDismissed(pending_permission_id_,
                                               tab_id_);
    pending_permission_id_.clear();
  }
}

void AbpPermissionObserver::TabPermissionObserver::
    OnPermissionRequestManagerDestructed() {
  manager_ = nullptr;
  // Owner will clean up via DetachFromTab when tab closes
}

// --- AbpPermissionObserver ---

AbpPermissionObserver::AbpPermissionObserver(AbpController* controller)
    : controller_(controller) {}

AbpPermissionObserver::~AbpPermissionObserver() = default;

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
  if (tab_observers_.count(tab_id))
    return;

  tab_observers_[tab_id] =
      std::make_unique<TabPermissionObserver>(this, tab_id, manager);

  VLOG(1) << "ABP: Permission observer attached to tab " << tab_id;
}

void AbpPermissionObserver::DetachFromTab(const std::string& tab_id) {
  tab_observers_.erase(tab_id);
  VLOG(1) << "ABP: Permission observer detached from tab " << tab_id;
}

bool AbpPermissionObserver::GrantPermission(const std::string& tab_id) {
  auto it = tab_observers_.find(tab_id);
  if (it == tab_observers_.end())
    return false;

  auto* obs = it->second.get();
  auto* manager = obs->manager();
  if (!manager || obs->pending_permission_id().empty())
    return false;

  VLOG(1) << "ABP: Granting permission in tab " << tab_id
          << " id=" << obs->pending_permission_id();

  obs->clear_pending_permission_id();
  manager->Accept();
  return true;
}

bool AbpPermissionObserver::DenyPermission(const std::string& tab_id) {
  auto it = tab_observers_.find(tab_id);
  if (it == tab_observers_.end())
    return false;

  auto* obs = it->second.get();
  auto* manager = obs->manager();
  if (!manager || obs->pending_permission_id().empty())
    return false;

  VLOG(1) << "ABP: Denying permission in tab " << tab_id
          << " id=" << obs->pending_permission_id();

  obs->clear_pending_permission_id();
  manager->Deny();
  return true;
}

std::string AbpPermissionObserver::GetPendingPermissionId(
    const std::string& tab_id) const {
  auto it = tab_observers_.find(tab_id);
  if (it == tab_observers_.end())
    return std::string();
  return it->second->pending_permission_id();
}

}  // namespace abp
