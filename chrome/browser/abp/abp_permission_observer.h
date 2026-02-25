// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_PERMISSION_OBSERVER_H_
#define CHROME_BROWSER_ABP_ABP_PERMISSION_OBSERVER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "components/permissions/permission_request_manager.h"

namespace content {
class WebContents;
}

namespace abp {

class AbpController;

class AbpPermissionObserver {
 public:
  explicit AbpPermissionObserver(AbpController* controller);
  ~AbpPermissionObserver();

  AbpPermissionObserver(const AbpPermissionObserver&) = delete;
  AbpPermissionObserver& operator=(const AbpPermissionObserver&) = delete;

  void AttachToTab(const std::string& tab_id,
                   content::WebContents* web_contents);
  void DetachFromTab(const std::string& tab_id);

  bool GrantPermission(const std::string& tab_id);
  bool DenyPermission(const std::string& tab_id);

  std::string GetPendingPermissionId(const std::string& tab_id) const;

 private:
  // Per-tab observer that implements the actual Observer interface.
  // Each tab gets its own so we know which tab's prompt triggered.
  class TabPermissionObserver
      : public permissions::PermissionRequestManager::Observer {
   public:
    TabPermissionObserver(AbpPermissionObserver* owner,
                          const std::string& tab_id,
                          permissions::PermissionRequestManager* manager);
    ~TabPermissionObserver() override;

    TabPermissionObserver(const TabPermissionObserver&) = delete;
    TabPermissionObserver& operator=(const TabPermissionObserver&) = delete;

    permissions::PermissionRequestManager* manager() { return manager_; }
    const std::string& pending_permission_id() const {
      return pending_permission_id_;
    }
    void set_pending_permission_id(const std::string& id) {
      pending_permission_id_ = id;
    }
    void clear_pending_permission_id() { pending_permission_id_.clear(); }

   private:
    // PermissionRequestManager::Observer:
    void OnPromptAdded() override;
    void OnPromptRemoved() override;
    void OnPermissionRequestManagerDestructed() override;

    raw_ptr<AbpPermissionObserver> owner_;
    std::string tab_id_;
    raw_ptr<permissions::PermissionRequestManager> manager_;
    std::string pending_permission_id_;
  };

  std::string GeneratePermissionId();

  raw_ptr<AbpController> controller_;
  std::map<std::string, std::unique_ptr<TabPermissionObserver>>
      tab_observers_;  // tab_id -> observer
  int next_permission_id_ = 1;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_PERMISSION_OBSERVER_H_
