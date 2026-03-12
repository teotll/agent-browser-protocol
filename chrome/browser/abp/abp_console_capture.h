// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ABP_ABP_CONSOLE_CAPTURE_H_
#define CHROME_BROWSER_ABP_ABP_CONSOLE_CAPTURE_H_

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/abp/abp_types.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom-forward.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace abp {

class AbpConsoleCapture;

// Per-tab WebContentsObserver that forwards console messages to the shared
// AbpConsoleCapture buffer. Created when a tab is inserted into the tab strip,
// destroyed when removed.
class AbpConsoleObserver : public content::WebContentsObserver {
 public:
  AbpConsoleObserver(content::WebContents* web_contents,
                     const std::string& tab_id,
                     AbpConsoleCapture* capture);
  ~AbpConsoleObserver() override;

  AbpConsoleObserver(const AbpConsoleObserver&) = delete;
  AbpConsoleObserver& operator=(const AbpConsoleObserver&) = delete;

  // content::WebContentsObserver:
  void OnDidAddMessageToConsole(
      content::RenderFrameHost* source_frame,
      blink::mojom::ConsoleMessageLevel log_level,
      const std::u16string& message,
      int32_t line_no,
      const std::u16string& source_id,
      const std::optional<std::u16string>& untrusted_stack_trace) override;

 private:
  std::string tab_id_;
  raw_ptr<AbpConsoleCapture> capture_;
};

// In-memory ring buffer for console messages captured via WebContentsObserver.
// All methods must be called on the UI thread.
class AbpConsoleCapture {
 public:
  static constexpr size_t kMaxBufferSize = 5000;

  AbpConsoleCapture();
  ~AbpConsoleCapture();

  AbpConsoleCapture(const AbpConsoleCapture&) = delete;
  AbpConsoleCapture& operator=(const AbpConsoleCapture&) = delete;

  // Ingest a console message. Called by AbpConsoleObserver.
  void OnConsoleMessage(const std::string& tab_id,
                        blink::mojom::ConsoleMessageLevel level,
                        const std::u16string& message,
                        int32_t line_number,
                        const std::u16string& source_id,
                        const std::optional<std::u16string>& stack_trace);

  // Query buffered entries with optional filters.
  // |min_level|: minimum severity ("verbose", "info", "warning", "error"),
  //              empty = all levels.
  // |pattern|: RE2 regex matched against message text (case-insensitive),
  //            empty = no filter.
  // |tab_id|: filter to specific tab, empty = all tabs.
  // |limit|: max entries to return (0 = no limit).
  // |after_id|: return entries with id > after_id (0 = all).
  std::vector<ConsoleEntry> Query(const std::string& tab_id,
                                  const std::string& min_level,
                                  const std::string& pattern,
                                  int limit,
                                  int64_t after_id) const;

  // Clear buffer. If |tab_id| is non-empty, only clear that tab's entries.
  // Returns number of entries cleared.
  size_t Clear(const std::string& tab_id);

  // Buffer stats for response metadata.
  size_t Size() const;
  int64_t OldestId() const;

 private:
  static std::string LevelToString(
      blink::mojom::ConsoleMessageLevel level);
  static int LevelPriority(const std::string& level);

  std::deque<ConsoleEntry> buffer_;
  int64_t next_id_ = 1;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CONSOLE_CAPTURE_H_
