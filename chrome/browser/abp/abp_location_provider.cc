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

// Static member definitions
bool AbpLocationProvider::s_has_stored_position_ = false;
double AbpLocationProvider::s_stored_latitude_ = 0.0;
double AbpLocationProvider::s_stored_longitude_ = 0.0;
double AbpLocationProvider::s_stored_accuracy_ = 100.0;

AbpLocationProvider::AbpLocationProvider()
    : provider_task_runner_(base::SingleThreadTaskRunner::GetCurrentDefault()) {
  g_instance = this;

  // If coordinates were stored before this instance was created, use them.
  if (s_has_stored_position_) {
    has_position_ = true;
    auto position = device::mojom::Geoposition::New();
    position->latitude = s_stored_latitude_;
    position->longitude = s_stored_longitude_;
    position->accuracy = s_stored_accuracy_;
    position->altitude = 0.0;
    position->altitude_accuracy = -1.0;
    position->heading = -1.0;
    position->speed = -1.0;
    position->timestamp = base::Time::Now();
    result_ =
        device::mojom::GeopositionResult::NewPosition(std::move(position));
    VLOG(1) << "ABP: Location provider created with stored position: "
            << s_stored_latitude_ << ", " << s_stored_longitude_;
  } else {
    // Initialize with "position unavailable" error
    result_ = device::mojom::GeopositionResult::NewError(
        device::mojom::GeopositionError::New(
            device::mojom::GeopositionErrorCode::kPositionUnavailable,
            /*error_message=*/"ABP: No mock location set",
            /*error_technical=*/""));
  }
}

AbpLocationProvider::~AbpLocationProvider() {
  if (g_instance == this)
    g_instance = nullptr;
}

// static
AbpLocationProvider* AbpLocationProvider::GetInstance() {
  return g_instance;
}

// static
void AbpLocationProvider::SetStoredPosition(double latitude,
                                            double longitude,
                                            double accuracy) {
  s_has_stored_position_ = true;
  s_stored_latitude_ = latitude;
  s_stored_longitude_ = longitude;
  s_stored_accuracy_ = accuracy;
  VLOG(1) << "ABP: Stored mock position: " << latitude << ", " << longitude
          << " accuracy=" << accuracy;
  // If a provider instance exists, update it too.
  if (g_instance) {
    g_instance->SetPosition(latitude, longitude, accuracy);
  }
}

// static
void AbpLocationProvider::ClearStoredPosition() {
  s_has_stored_position_ = false;
  s_stored_latitude_ = 0.0;
  s_stored_longitude_ = 0.0;
  s_stored_accuracy_ = 100.0;
  VLOG(1) << "ABP: Cleared stored mock position";
  // If a provider instance exists, clear it too.
  if (g_instance) {
    g_instance->ClearPosition();
  }
}

// static
bool AbpLocationProvider::HasStoredPosition() {
  return s_has_stored_position_;
}

// static
double AbpLocationProvider::stored_latitude() {
  return s_stored_latitude_;
}

// static
double AbpLocationProvider::stored_longitude() {
  return s_stored_longitude_;
}

// static
double AbpLocationProvider::stored_accuracy() {
  return s_stored_accuracy_;
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
