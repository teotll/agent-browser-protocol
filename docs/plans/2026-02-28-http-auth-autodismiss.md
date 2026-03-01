# HTTP Auth Auto-Dismiss Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Auto-dismiss HTTP auth dialogs (401/407) and emit an `http_auth_dismissed` event so agents know it happened.

**Architecture:** Intercept at `ChromeContentBrowserClient::CreateLoginDelegate()` with a custom `LoginDelegate` that immediately cancels auth and notifies `AbpController`, which emits the event via `EmitPopupEvent` and records to history.

**Tech Stack:** C++ (Chromium content API, `LoginDelegate`, `AuthChallengeInfo`)

---

### Task 1: Create AbpLoginDelegate

**Files:**
- Create: `chrome/browser/abp/abp_login_delegate.h`
- Create: `chrome/browser/abp/abp_login_delegate.cc`

**Step 1: Create the header**

```cpp
// chrome/browser/abp/abp_login_delegate.h
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_LOGIN_DELEGATE_H_
#define CHROME_BROWSER_ABP_ABP_LOGIN_DELEGATE_H_

#include "content/public/browser/login_delegate.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"

namespace abp {

class AbpController;

// LoginDelegate that immediately cancels HTTP auth challenges.
// Emits an "http_auth_dismissed" event via AbpController so agents
// know the auth dialog was suppressed.
class AbpLoginDelegate : public content::LoginDelegate {
 public:
  AbpLoginDelegate(
      const std::string& scheme,
      const std::string& realm,
      const std::string& host,
      bool is_proxy,
      const std::string& path,
      base::WeakPtr<AbpController> controller,
      content::LoginDelegate::LoginAuthRequiredCallback auth_callback);
  ~AbpLoginDelegate() override;

 private:
  void CancelAuthAndNotify();

  std::string scheme_;
  std::string realm_;
  std::string host_;
  bool is_proxy_;
  std::string path_;
  base::WeakPtr<AbpController> controller_;
  content::LoginDelegate::LoginAuthRequiredCallback auth_callback_;
  base::WeakPtrFactory<AbpLoginDelegate> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_LOGIN_DELEGATE_H_
```

**Step 2: Create the implementation**

```cpp
// chrome/browser/abp/abp_login_delegate.cc
// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_login_delegate.h"

#include "base/logging.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/abp/abp_controller.h"

namespace abp {

AbpLoginDelegate::AbpLoginDelegate(
    const std::string& scheme,
    const std::string& realm,
    const std::string& host,
    bool is_proxy,
    const std::string& path,
    base::WeakPtr<AbpController> controller,
    content::LoginDelegate::LoginAuthRequiredCallback auth_callback)
    : scheme_(scheme),
      realm_(realm),
      host_(host),
      is_proxy_(is_proxy),
      path_(path),
      controller_(std::move(controller)),
      auth_callback_(std::move(auth_callback)) {
  // Post cancel to avoid reentrancy — the network stack is still setting
  // up when CreateLoginDelegate returns.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpLoginDelegate::CancelAuthAndNotify,
                     weak_factory_.GetWeakPtr()));
}

AbpLoginDelegate::~AbpLoginDelegate() = default;

void AbpLoginDelegate::CancelAuthAndNotify() {
  VLOG(1) << "ABP: Auto-dismissing HTTP auth for " << host_
          << " (scheme=" << scheme_ << ", realm=" << realm_ << ")";

  // Cancel the auth challenge (shows error page for 401, retries without
  // credentials for proxy 407).
  if (auth_callback_) {
    std::move(auth_callback_).Run(std::nullopt);
  }

  // Emit event so agent knows what happened.
  if (controller_) {
    base::Value::Dict event_data;
    event_data.Set("scheme", scheme_);
    event_data.Set("realm", realm_);
    event_data.Set("host", host_);
    event_data.Set("is_proxy", is_proxy_);
    event_data.Set("path", path_);
    controller_->EmitPopupEvent("http_auth_dismissed", std::move(event_data));
  }
}

}  // namespace abp
```

**Step 3: Commit**

```bash
git add chrome/browser/abp/abp_login_delegate.h chrome/browser/abp/abp_login_delegate.cc
git commit -m "feat(abp): add AbpLoginDelegate for HTTP auth auto-dismiss"
```

---

### Task 2: Add to BUILD.gn

**Files:**
- Modify: `chrome/browser/abp/BUILD.gn:18-50`

**Step 1: Add source files**

Add `abp_login_delegate.cc` and `abp_login_delegate.h` to the `sources` list in the `"abp"` source_set, after the existing `abp_location_provider` entries:

```
    "abp_location_provider.cc",
    "abp_location_provider.h",
    "abp_login_delegate.cc",
    "abp_login_delegate.h",
    "abp_types.h",
```

**Step 2: Commit**

```bash
git add chrome/browser/abp/BUILD.gn
git commit -m "build(abp): add abp_login_delegate to BUILD.gn"
```

---

### Task 3: Wire up in ChromeContentBrowserClient

**Files:**
- Modify: `chrome/browser/chrome_content_browser_client.cc:54,6860-6868`

**Step 1: Add include**

After the existing `#include "chrome/browser/abp/abp_location_provider.h"` (line 54), add:

```cpp
#include "chrome/browser/abp/abp_login_delegate.h"
```

**Step 2: Add ABP interception in CreateLoginDelegate**

After the `#endif  // BUILDFLAG(IS_CHROMEOS)` block (line 6860) and before the `http_auth_coordinator_` block (line 6862), add:

```cpp
  // ABP: auto-dismiss HTTP auth dialogs and emit event.
  // Skip in test mode so browser tests can test auth normally.
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch("test-type")) {
    return std::make_unique<abp::AbpLoginDelegate>(
        auth_info.scheme, auth_info.realm,
        auth_info.challenger.Serialize(), auth_info.is_proxy,
        auth_info.path,
        /* controller WeakPtr — needs accessor */
        abp::AbpController::GetInstanceForTesting()
            ? abp::AbpController::GetInstanceForTesting()->GetWeakPtr()
            : base::WeakPtr<abp::AbpController>(),
        std::move(auth_required_callback));
  }
```

Wait — `GetInstanceForTesting()` is for tests. Let me check how the controller is accessed from `ChromeContentBrowserClient`.

**Step 2 (revised): Check controller access pattern**

The geolocation override at line 3834 doesn't need the controller — it just creates a provider. But we need a `WeakPtr<AbpController>`. The controller is owned by `AbpHttpServer`. Let me check if there's a global accessor.

Looking at the codebase, `AbpController::GetInstanceForTesting()` returns a static `instance_for_testing_` pointer. In production, the controller is created by `AbpHttpServer`. We need a production-safe static accessor.

**Revised approach:** Add a static `GetInstance()` method to `AbpController` (similar to the test one but always set). The controller already sets `instance_for_testing_` in the constructor — we just rename/reuse it as a general instance pointer.

**Step 2a: Add static instance accessor to AbpController**

In `chrome/browser/abp/abp_controller.h`, the existing `GetInstanceForTesting()` static method and `instance_for_testing_` field already exist. Check the constructor:

```cpp
// In abp_controller.cc constructor:
static AbpController* AbpController::instance_for_testing_ = nullptr;

AbpController::AbpController() {
  instance_for_testing_ = this;
  // ...
}
```

The static instance is already set in production. `GetInstanceForTesting()` just returns it. So we can safely use it from `CreateLoginDelegate`. The name is misleading but functional.

**Step 2b: Add the interception**

After `#endif  // BUILDFLAG(IS_CHROMEOS)` (line 6860) and before the `http_auth_coordinator_` block:

```cpp
  // ABP: auto-dismiss HTTP auth dialogs and emit event.
  // Skip in test mode so browser tests can test auth normally.
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch("test-type")) {
    auto* controller = abp::AbpController::GetInstanceForTesting();
    return std::make_unique<abp::AbpLoginDelegate>(
        auth_info.scheme, auth_info.realm,
        auth_info.challenger.Serialize(), auth_info.is_proxy,
        auth_info.path,
        controller ? controller->GetWeakPtr()
                   : base::WeakPtr<abp::AbpController>(),
        std::move(auth_required_callback));
  }
```

**Step 3: Add controller header include**

After the `abp_login_delegate.h` include, add:

```cpp
#include "chrome/browser/abp/abp_controller.h"
```

**Step 4: Commit**

```bash
git add chrome/browser/chrome_content_browser_client.cc
git commit -m "feat(abp): intercept HTTP auth in CreateLoginDelegate"
```

---

### Task 4: Build and verify

**Step 1: Build**

```bash
autoninja -C out/Default chrome
```

Expected: Build succeeds with no errors.

**Step 2: Manual test**

Start ABP and navigate to a site requiring HTTP auth (e.g. `httpbin.org/basic-auth/user/pass`):

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/test_auth --no-first-run &
sleep 3

# Create tab and navigate to auth-protected page
curl -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"https://httpbin.org/basic-auth/user/pass"}'
```

Expected: No auth dialog appears. The page shows a 401 error response. ABP logs show `ABP: Auto-dismissing HTTP auth for...`.

**Step 3: Commit (if any fixes needed)**

```bash
git add -u
git commit -m "fix(abp): address build/test issues for HTTP auth auto-dismiss"
```
