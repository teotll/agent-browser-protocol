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

class AbpLocationProvider : public device::LocationProvider {
 public:
  AbpLocationProvider();
  ~AbpLocationProvider() override;

  AbpLocationProvider(const AbpLocationProvider&) = delete;
  AbpLocationProvider& operator=(const AbpLocationProvider&) = delete;

  void SetPosition(double latitude, double longitude, double accuracy);
  void ClearPosition();
  bool has_position() const { return has_position_; }

  // device::LocationProvider:
  void FillDiagnostics(device::mojom::GeolocationDiagnostics& diagnostics) override;
  void SetUpdateCallback(const LocationProviderUpdateCallback& callback) override;
  void StartProvider(bool high_accuracy) override;
  void StopProvider() override;
  const device::mojom::GeopositionResult* GetPosition() override;
  void OnPermissionGranted() override;

  static AbpLocationProvider* GetInstance();

  // Static coordinate storage — persists independently of provider instances.
  // Allows SetGeolocation API to work before any page requests geolocation.
  static void SetStoredPosition(double latitude,
                                double longitude,
                                double accuracy);
  static void ClearStoredPosition();
  static bool HasStoredPosition();
  static double stored_latitude();
  static double stored_longitude();
  static double stored_accuracy();

 private:
  void NotifyListeners();

  device::mojom::GeolocationDiagnostics::ProviderState state_ =
      device::mojom::GeolocationDiagnostics::ProviderState::kStopped;
  bool is_permission_granted_ = false;
  bool has_position_ = false;
  device::mojom::GeopositionResultPtr result_;
  LocationProviderUpdateCallback callback_;
  scoped_refptr<base::SingleThreadTaskRunner> provider_task_runner_;

  // Static storage for mock coordinates
  static bool s_has_stored_position_;
  static double s_stored_latitude_;
  static double s_stored_longitude_;
  static double s_stored_accuracy_;

  base::WeakPtrFactory<AbpLocationProvider> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_LOCATION_PROVIDER_H_
