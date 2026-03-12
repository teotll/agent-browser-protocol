// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_console_capture.h"

#include <algorithm>

#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom.h"
#include "third_party/re2/src/re2/re2.h"

namespace abp {

// --- AbpConsoleObserver ---

AbpConsoleObserver::AbpConsoleObserver(content::WebContents* web_contents,
                                       const std::string& tab_id,
                                       AbpConsoleCapture* capture)
    : content::WebContentsObserver(web_contents),
      tab_id_(tab_id),
      capture_(capture) {}

AbpConsoleObserver::~AbpConsoleObserver() = default;

void AbpConsoleObserver::OnDidAddMessageToConsole(
    content::RenderFrameHost* source_frame,
    blink::mojom::ConsoleMessageLevel log_level,
    const std::u16string& message,
    int32_t line_no,
    const std::u16string& source_id,
    const std::optional<std::u16string>& untrusted_stack_trace) {
  if (capture_) {
    capture_->OnConsoleMessage(tab_id_, log_level, message, line_no, source_id,
                               untrusted_stack_trace);
  }
}

// --- AbpConsoleCapture ---

AbpConsoleCapture::AbpConsoleCapture() = default;
AbpConsoleCapture::~AbpConsoleCapture() = default;

void AbpConsoleCapture::OnConsoleMessage(
    const std::string& tab_id,
    blink::mojom::ConsoleMessageLevel level,
    const std::u16string& message,
    int32_t line_number,
    const std::u16string& source_id,
    const std::optional<std::u16string>& stack_trace) {
  ConsoleEntry entry;
  entry.id = next_id_++;
  entry.tab_id = tab_id;
  entry.level = LevelToString(level);
  entry.message = base::UTF16ToUTF8(message);
  entry.line_number = line_number;
  entry.source_url = base::UTF16ToUTF8(source_id);
  entry.stack_trace =
      stack_trace.has_value() ? base::UTF16ToUTF8(*stack_trace) : "";
  entry.timestamp_ms =
      (base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds();

  buffer_.push_back(std::move(entry));

  // FIFO eviction
  while (buffer_.size() > kMaxBufferSize) {
    buffer_.pop_front();
  }
}

std::vector<ConsoleEntry> AbpConsoleCapture::Query(
    const std::string& tab_id,
    const std::string& min_level,
    const std::string& pattern,
    int limit,
    int64_t after_id) const {
  int min_priority = min_level.empty() ? 0 : LevelPriority(min_level);

  // Compile regex if provided. RE2 guarantees linear-time matching.
  std::unique_ptr<RE2> regex;
  if (!pattern.empty()) {
    RE2::Options opts;
    opts.set_case_sensitive(false);
    regex = std::make_unique<RE2>(pattern, opts);
    if (!regex->ok()) {
      return {};  // Invalid regex — caller should check and return 400
    }
  }

  std::vector<ConsoleEntry> results;
  for (const auto& entry : buffer_) {
    if (entry.id <= after_id) {
      continue;
    }
    if (!tab_id.empty() && entry.tab_id != tab_id) {
      continue;
    }
    if (LevelPriority(entry.level) < min_priority) {
      continue;
    }
    if (regex && !RE2::PartialMatch(entry.message, *regex)) {
      continue;
    }
    results.push_back(entry);
    if (limit > 0 && static_cast<int>(results.size()) >= limit) {
      break;
    }
  }
  return results;
}

size_t AbpConsoleCapture::Clear(const std::string& tab_id) {
  if (tab_id.empty()) {
    size_t count = buffer_.size();
    buffer_.clear();
    return count;
  }
  size_t before = buffer_.size();
  std::erase_if(buffer_,
                [&tab_id](const ConsoleEntry& e) {
                  return e.tab_id == tab_id;
                });
  return before - buffer_.size();
}

size_t AbpConsoleCapture::Size() const {
  return buffer_.size();
}

int64_t AbpConsoleCapture::OldestId() const {
  return buffer_.empty() ? 0 : buffer_.front().id;
}

// static
std::string AbpConsoleCapture::LevelToString(
    blink::mojom::ConsoleMessageLevel level) {
  switch (level) {
    case blink::mojom::ConsoleMessageLevel::kVerbose:
      return "verbose";
    case blink::mojom::ConsoleMessageLevel::kInfo:
      return "info";
    case blink::mojom::ConsoleMessageLevel::kWarning:
      return "warning";
    case blink::mojom::ConsoleMessageLevel::kError:
      return "error";
  }
  return "info";
}

// static
int AbpConsoleCapture::LevelPriority(const std::string& level) {
  if (level == "verbose") return 0;
  if (level == "info") return 1;
  if (level == "warning") return 2;
  if (level == "error") return 3;
  return 0;
}

}  // namespace abp
