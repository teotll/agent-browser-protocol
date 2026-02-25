// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_SYSTEM_GEOLOCATION_SOURCE_H_
#define CHROME_BROWSER_ABP_ABP_SYSTEM_GEOLOCATION_SOURCE_H_

#include <memory>

#include "build/build_config.h"
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#include "services/device/public/cpp/geolocation/system_geolocation_source.h"

namespace abp {

// A SystemGeolocationSource for ABP that starts with kNotDetermined (so Chrome
// shows web permission prompts) and transitions to kAllowed when
// GrantSystemPermission() is called (so the provider can deliver coordinates).
// Never calls CoreLocation, preventing the macOS system location dialog.
class AbpSystemGeolocationSource : public device::SystemGeolocationSource {
 public:
  AbpSystemGeolocationSource();
  ~AbpSystemGeolocationSource() override;

  void RegisterPermissionUpdateCallback(
      PermissionUpdateCallback callback) override;

#if BUILDFLAG(IS_APPLE)
  void StartWatchingPosition(bool high_accuracy) override;
  void StopWatchingPosition() override;
  void AddPositionUpdateObserver(PositionObserver* observer) override;
  void RemovePositionUpdateObserver(PositionObserver* observer) override;
#endif

#if BUILDFLAG(IS_APPLE) || BUILDFLAG(IS_WIN)
  void RequestPermission() override;
#endif

  // Transition system permission from kNotDetermined to kAllowed.
  // Called by AbpController when granting a geolocation web permission.
  static void GrantSystemPermission();

  static std::unique_ptr<device::GeolocationSystemPermissionManager>
  CreatePermissionManager();

 private:
  PermissionUpdateCallback permission_callback_;
  static AbpSystemGeolocationSource* g_instance_;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_SYSTEM_GEOLOCATION_SOURCE_H_
