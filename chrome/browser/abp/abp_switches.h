// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_SWITCHES_H_
#define CHROME_BROWSER_ABP_ABP_SWITCHES_H_

namespace abp::switches {

// Port for HTTP server (default: 8222)
extern const char kAbpPort[];

// Path to ABP config file (default: ~/.config/chromium/abp_config.json)
extern const char kAbpConfig[];

// Disable execution control (Debugger.pause + virtual time) - enabled by default
extern const char kAbpDisablePause[];

// Session directory for storing screenshots, database, and logs
// Default: /tmp/abp-<UUID>
extern const char kAbpSessionDir[];

// Window size as "width,height" (default: 1280,887)
// Also prevents user resizing of the browser window.
extern const char kAbpWindowSize[];

// Default zoom factor as a decimal (default: 1.0 = 100%)
extern const char kAbpZoom[];

// Minimum wait time in ms before network snapshot (default: 250)
extern const char kAbpMinWait[];

// Request tracking timeout in ms (default: 1000)
extern const char kAbpTrackingTimeout[];

// Post-network settle time in ms (default: 750)
extern const char kAbpPostSettle[];

}  // namespace abp::switches

#endif  // CHROME_BROWSER_ABP_ABP_SWITCHES_H_
