// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_CONFIG_H_
#define CHROME_BROWSER_ABP_ABP_CONFIG_H_

#include <string>

#include "base/files/file_path.h"
#include "base/time/time.h"

namespace abp {

// Configuration for ABP history system.
// Loaded from ~/.config/chromium/abp_config.json or --abp-config flag.
struct AbpConfig {
  // Session directory - stores database, screenshots, and logs
  // Default: /tmp/abp-<UUID>
  base::FilePath session_dir;

  struct HistoryConfig {
    bool enabled = true;
    base::FilePath database_path;

    struct ScreenshotConfig {
      bool enabled = true;
      base::FilePath directory;
    };
    ScreenshotConfig screenshots;
  };
  HistoryConfig history;

  struct TimingConfig {
    // Phase 1: JS hook window before network snapshot
    base::TimeDelta min_wait = base::Milliseconds(150);
    // Phase 2: How long to track in-flight requests
    base::TimeDelta tracking_timeout = base::Milliseconds(1000);
    // Phase 3: Settle after tracked requests complete
    base::TimeDelta post_settle = base::Milliseconds(350);
  };
  TimingConfig timing;

  // Returns the default configuration with a new UUID-based session directory
  static AbpConfig GetDefaults();

  // Returns configuration with a specific session directory
  static AbpConfig GetDefaultsWithSessionDir(const base::FilePath& session_dir);
};

// Loads configuration from file or returns defaults.
// Checks --abp-config flag first, then ~/.config/chromium/abp_config.json.
// Returns defaults if no config file exists.
AbpConfig LoadAbpConfig();

// Loads configuration from a specific file path.
// Returns defaults if file doesn't exist or parse fails.
AbpConfig LoadAbpConfigFromFile(const base::FilePath& config_path);

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CONFIG_H_
