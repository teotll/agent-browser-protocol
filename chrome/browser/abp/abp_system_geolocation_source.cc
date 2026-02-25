// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_system_geolocation_source.h"

#include <memory>

#include "base/logging.h"

namespace abp {

AbpSystemGeolocationSource* AbpSystemGeolocationSource::g_instance_ = nullptr;

AbpSystemGeolocationSource::AbpSystemGeolocationSource() {
  g_instance_ = this;
}

AbpSystemGeolocationSource::~AbpSystemGeolocationSource() {
  if (g_instance_ == this)
    g_instance_ = nullptr;
}

void AbpSystemGeolocationSource::RegisterPermissionUpdateCallback(
    PermissionUpdateCallback callback) {
  permission_callback_ = callback;
  // Start with kNotDetermined so CanPrompt() returns true and Chrome shows
  // web permission prompts for geolocation.
  VLOG(1) << "ABP: AbpSystemGeolocationSource registering callback, "
           << "reporting kNotDetermined";
  callback.Run(device::LocationSystemPermissionStatus::kNotDetermined);
}

#if BUILDFLAG(IS_APPLE)
void AbpSystemGeolocationSource::StartWatchingPosition(bool high_accuracy) {}
void AbpSystemGeolocationSource::StopWatchingPosition() {}
void AbpSystemGeolocationSource::AddPositionUpdateObserver(
    PositionObserver* observer) {}
void AbpSystemGeolocationSource::RemovePositionUpdateObserver(
    PositionObserver* observer) {}
#endif

#if BUILDFLAG(IS_APPLE) || BUILDFLAG(IS_WIN)
void AbpSystemGeolocationSource::RequestPermission() {
  // The real macOS source would call [CLLocationManager
  // requestWhenInUseAuthorization] here, showing a system dialog. We skip it
  // entirely. The transition to kAllowed happens via GrantSystemPermission().
}
#endif

// static
void AbpSystemGeolocationSource::GrantSystemPermission() {
  if (g_instance_ && g_instance_->permission_callback_) {
    VLOG(1) << "ABP: Transitioning system geolocation permission to kAllowed";
    g_instance_->permission_callback_.Run(
        device::LocationSystemPermissionStatus::kAllowed);
  }
}

// static
std::unique_ptr<device::GeolocationSystemPermissionManager>
AbpSystemGeolocationSource::CreatePermissionManager() {
  return std::make_unique<device::GeolocationSystemPermissionManager>(
      std::make_unique<AbpSystemGeolocationSource>());
}

}  // namespace abp
