// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_LOGIN_DELEGATE_H_
#define CHROME_BROWSER_ABP_ABP_LOGIN_DELEGATE_H_

#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/login_delegate.h"

namespace content {
class WebContents;
}

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
      content::WebContents* web_contents,
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
  std::string tab_id_;
  base::WeakPtr<AbpController> controller_;
  content::LoginDelegate::LoginAuthRequiredCallback auth_callback_;
  base::WeakPtrFactory<AbpLoginDelegate> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_LOGIN_DELEGATE_H_
