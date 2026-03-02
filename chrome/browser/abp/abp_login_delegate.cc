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
    content::WebContents* web_contents,
    base::WeakPtr<AbpController> controller,
    content::LoginDelegate::LoginAuthRequiredCallback auth_callback)
    : scheme_(scheme),
      realm_(realm),
      host_(host),
      is_proxy_(is_proxy),
      path_(path),
      controller_(std::move(controller)),
      auth_callback_(std::move(auth_callback)) {
  // Resolve tab_id eagerly — WebContents may not outlive this delegate
  // (per content_browser_client.h documentation).
  if (web_contents && controller_) {
    tab_id_ = controller_->GetTabIdForWebContents(web_contents);
  }
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&AbpLoginDelegate::CancelAuthAndNotify,
                     weak_factory_.GetWeakPtr()));
}

AbpLoginDelegate::~AbpLoginDelegate() = default;

void AbpLoginDelegate::CancelAuthAndNotify() {
  VLOG(1) << "ABP: Auto-dismissing HTTP auth for " << host_
          << " (scheme=" << scheme_ << ", realm=" << realm_ << ")";

  if (auth_callback_) {
    std::move(auth_callback_).Run(std::nullopt);
  }

  if (controller_) {
    base::Value::Dict event_data;
    event_data.Set("scheme", scheme_);
    event_data.Set("realm", realm_);
    event_data.Set("host", host_);
    event_data.Set("is_proxy", is_proxy_);
    event_data.Set("path", path_);

    controller_->OnHttpAuthDismissed(tab_id_, std::move(event_data));
  }
}

}  // namespace abp
