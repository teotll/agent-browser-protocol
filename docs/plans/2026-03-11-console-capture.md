# Console Capture Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose JavaScript console messages (all levels, including CORS/CSP) via MCP tool and REST API, using Chromium's native WebContentsObserver — no CDP, no JS injection, not fingerprintable.

**Architecture:** Remove the `kNetwork` source filter in Blink's `FrameConsole` so all console messages reach the browser process via Mojo IPC. A per-tab `AbpConsoleObserver` (WebContentsObserver) forwards messages to a shared `AbpConsoleCapture` ring buffer (5000 entries, FIFO). Queried via `GET /api/v1/console` REST endpoint and `browser_console` MCP tool.

**Tech Stack:** C++ (Chromium), RE2 regex, base::Value JSON

**Spec:** `docs/plans/2026-03-11-console-capture-design.md`

---

## Chunk 1: Core Capture Infrastructure

### Task 1: Renderer-side change — remove kNetwork filter

**Files:**
- Modify: `third_party/blink/renderer/core/frame/frame_console.cc:74-75`

- [ ] **Step 1: Remove the kNetwork source filter**

In `FrameConsole::ReportMessageToClient` at line 74, replace the early-return:

```cpp
// Before (lines 74-75):
  if (source == mojom::blink::ConsoleMessageSource::kNetwork)
    return;

// After:
  // ABP: Forward network-source console messages (CORS, CSP, mixed content)
  // to the browser process via DidAddMessageToConsole. Stock Chromium filters
  // these out since they're redundant with the Log domain, but ABP captures
  // console messages via WebContentsObserver to avoid enabling CDP domains
  // that could be fingerprinted by page JavaScript.
```

Simply delete lines 74-75 and add the comment. The surrounding code at lines 77-99 already handles all source types correctly — `kNetwork` messages will fall into the `else` branch at line 90.

- [ ] **Step 2: Commit**

```bash
git add third_party/blink/renderer/core/frame/frame_console.cc
git commit -m "feat(abp): forward network-source console messages to browser process

Remove the kNetwork early-return in FrameConsole::ReportMessageToClient
so CORS, CSP, and mixed content messages reach the browser via
DidAddMessageToConsole Mojo IPC. ABP captures console messages via
WebContentsObserver to avoid enabling CDP domains that pages could
fingerprint."
```

---

### Task 2: Add ConsoleEntry struct to abp_types.h

**Files:**
- Modify: `chrome/browser/abp/abp_types.h:105-107`

- [ ] **Step 1: Add ConsoleEntry struct**

Add after `NetworkQueryFilter` (after line 105), before the closing `}  // namespace abp`:

```cpp
// A single console message captured from the renderer via
// WebContentsObserver::OnDidAddMessageToConsole.
struct ConsoleEntry {
  int64_t id = 0;               // Monotonic sequence number for pagination
  std::string tab_id;            // DevToolsAgentHost ID of source tab
  std::string level;             // "verbose", "info", "warning", "error"
  std::string message;           // Message text
  int32_t line_number = 0;       // Source line number (0 if unavailable)
  std::string source_url;        // Script URL or empty
  std::string stack_trace;       // Stack trace (errors only, if available)
  int64_t timestamp_ms = 0;      // Wall clock milliseconds since epoch

  // Serialize to JSON dict for API responses.
  base::Value::Dict ToDict() const;
};
```

- [ ] **Step 2: Add ToDict() implementation in abp_types.cc**

In `chrome/browser/abp/abp_types.cc`, add at the end (before the closing `}  // namespace abp`):

```cpp
base::Value::Dict ConsoleEntry::ToDict() const {
  base::Value::Dict dict;
  dict.Set("id", static_cast<double>(id));
  dict.Set("tab_id", tab_id);
  dict.Set("level", level);
  dict.Set("message", message);
  dict.Set("line_number", line_number);
  dict.Set("source_url", source_url);
  dict.Set("stack_trace", stack_trace);
  dict.Set("timestamp_ms", static_cast<double>(timestamp_ms));
  return dict;
}
```

- [ ] **Step 3: Commit**

```bash
git add chrome/browser/abp/abp_types.h chrome/browser/abp/abp_types.cc
git commit -m "feat(abp): add ConsoleEntry struct for console capture"
```

---

### Task 3: Create AbpConsoleCapture and AbpConsoleObserver

**Files:**
- Create: `chrome/browser/abp/abp_console_capture.h`
- Create: `chrome/browser/abp/abp_console_capture.cc`
- Modify: `chrome/browser/abp/BUILD.gn:18-63`

- [ ] **Step 1: Create abp_console_capture.h**

```cpp
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
```

- [ ] **Step 2: Create abp_console_capture.cc**

```cpp
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
```

- [ ] **Step 3: Add to BUILD.gn**

In `chrome/browser/abp/BUILD.gn`, add the new source files to the `sources` list (alphabetically, after `abp_config.h`):

```gn
    "abp_console_capture.cc",
    "abp_console_capture.h",
```

Also add RE2 dependency. In the `deps` section (around line 72), add:

```gn
    "//third_party/re2",
```

- [ ] **Step 4: Verify build compiles**

Run: `autoninja -C out/Default chrome`

Expected: Compiles with no errors. The new files are compiled but not yet used.

- [ ] **Step 5: Commit**

```bash
git add chrome/browser/abp/abp_console_capture.h \
        chrome/browser/abp/abp_console_capture.cc \
        chrome/browser/abp/BUILD.gn
git commit -m "feat(abp): add AbpConsoleCapture ring buffer and AbpConsoleObserver

5000-entry FIFO buffer for console messages captured via
WebContentsObserver. Supports query with level, tab_id, regex pattern,
limit, and after_id filters. Per-tab AbpConsoleObserver forwards
OnDidAddMessageToConsole to the shared buffer."
```

---

## Chunk 2: Controller Integration and REST API

### Task 4: Wire AbpConsoleCapture into AbpController

**Note:** `abp_http_server.cc` does not need modification. The HTTP server already routes all `/api/v1/*` requests (except `/api/v1/history`) to `AbpController::HandleRequest` (line 295). We only need to add the `console` route inside the controller's request dispatcher.

**Files:**
- Modify: `chrome/browser/abp/abp_controller.h:630-638` (add declarations)
- Modify: `chrome/browser/abp/abp_controller.h:1149-1152` (add members)
- Modify: `chrome/browser/abp/abp_controller.cc` (tab observer lifecycle, HandleConsoleRequest, routing)

- [ ] **Step 1: Add includes and forward declarations to controller header**

In `chrome/browser/abp/abp_controller.h`, add a forward declaration near the top of the `abp` namespace (where other forward declarations are):

```cpp
class AbpConsoleCapture;
class AbpConsoleObserver;
```

Add the include for the console capture header:

```cpp
#include "chrome/browser/abp/abp_console_capture.h"
```

- [ ] **Step 2: Add handler declarations to controller header**

After the network capture endpoint declarations (after line 638, before the `CaptureActionScreenshotCdp` comment at line 641), add:

```cpp
  // Console capture endpoints
  void HandleConsoleQuery(const std::string& query_string,
                          ResponseCallback callback);
  void HandleConsoleClear(const std::string& query_string,
                          ResponseCallback callback);
```

- [ ] **Step 3: Add member variables to controller header**

After `permission_observer_` declaration (line 1152), add:

```cpp
  // Console capture buffer (shared across all tabs)
  std::unique_ptr<AbpConsoleCapture> console_capture_;

  // Per-tab console observers (keyed by tab_id)
  std::map<std::string, std::unique_ptr<AbpConsoleObserver>> console_observers_;
```

- [ ] **Step 4: Create console_capture_ in controller constructor**

In `abp_controller.cc`, in the constructor or initialization section where other members are created (near where `event_collector_` is created), add:

```cpp
  console_capture_ = std::make_unique<AbpConsoleCapture>();
```

- [ ] **Step 5: Manage per-tab AbpConsoleObserver lifecycle**

Console observers must be attached from the moment of tab creation (not lazily via `GetOrCreateTabState`), so we don't miss early console messages.

In `abp_controller.cc`, find the `TabStripModelObserver` callback `OnTabInserted` or the existing tab-tracking code that fires when a new tab is added to the strip. In the existing `OnTabStripModelChanged` (line 4728), the method currently only handles active tab changes (selection). Find the `TabStripModelChange::kInserted` handling path (or the equivalent — search for `change.type() == TabStripModelChange::kInserted` or the existing tab insertion handler). If no insertion handler exists, add one.

Add console observer creation when a tab is inserted:

```cpp
  // In the tab insertion path:
  if (change.type() == TabStripModelChange::kInserted) {
    for (const auto& contents : change.GetInsert()->contents) {
      auto host = content::DevToolsAgentHost::GetOrCreateFor(
          contents.contents);
      std::string tab_id = host->GetId();
      if (console_capture_ &&
          console_observers_.find(tab_id) == console_observers_.end()) {
        console_observers_[tab_id] = std::make_unique<AbpConsoleObserver>(
            contents.contents, tab_id, console_capture_.get());
      }
    }
  }
```

For tab removal, find where tabs are cleaned up on close (search for `tab_states_.erase` or `kRemoved` handling). Add before the erase:

```cpp
  console_observers_.erase(tab_id);
```

Also handle initial tabs that exist before the observer is registered. In the controller's initialization (after `console_capture_` is created and after the `TabStripModel` is observed), iterate existing tabs:

```cpp
  // Attach console observers to any tabs that already exist.
  if (console_capture_ && browser_ && browser_->tab_strip_model()) {
    TabStripModel* model = browser_->tab_strip_model();
    for (int i = 0; i < model->count(); ++i) {
      content::WebContents* wc = model->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      std::string tab_id = host->GetId();
      console_observers_[tab_id] = std::make_unique<AbpConsoleObserver>(
          wc, tab_id, console_capture_.get());
    }
  }
```

- [ ] **Step 6: Add HandleConsoleQuery implementation**

In `abp_controller.cc`, add after `HandleNetworkClear`:

```cpp
void AbpController::HandleConsoleQuery(const std::string& query_string,
                                       ResponseCallback callback) {
  if (!console_capture_) {
    SendError(503, "Console capture not initialized", std::move(callback));
    return;
  }

  std::string tab_id = GetQueryParam(query_string, "tab_id");
  std::string level = GetQueryParam(query_string, "level");
  std::string pattern = GetQueryParam(query_string, "pattern");
  std::string limit_str = GetQueryParam(query_string, "limit");
  std::string after_id_str = GetQueryParam(query_string, "after_id");

  int limit = 100;
  if (!limit_str.empty()) {
    base::StringToInt(limit_str, &limit);
  }
  int64_t after_id = 0;
  if (!after_id_str.empty()) {
    base::StringToInt64(after_id_str, &after_id);
  }

  // Validate regex before querying.
  if (!pattern.empty()) {
    RE2 test_regex(pattern);
    if (!test_regex.ok()) {
      SendError(400, "Invalid regex pattern: " + test_regex.error(),
                std::move(callback));
      return;
    }
  }

  auto entries = console_capture_->Query(tab_id, level, pattern, limit,
                                         after_id);

  base::Value::List entries_list;
  for (const auto& entry : entries) {
    entries_list.Append(entry.ToDict());
  }

  base::Value::Dict response;
  response.Set("entries", std::move(entries_list));
  response.Set("total_buffered",
               static_cast<int>(console_capture_->Size()));
  response.Set("oldest_id",
               static_cast<double>(console_capture_->OldestId()));

  std::string json;
  base::JSONWriter::Write(response, &json);
  std::move(callback).Run(200, "application/json", std::move(json));
}

void AbpController::HandleConsoleClear(const std::string& query_string,
                                       ResponseCallback callback) {
  if (!console_capture_) {
    SendError(503, "Console capture not initialized", std::move(callback));
    return;
  }

  std::string tab_id = GetQueryParam(query_string, "tab_id");
  size_t cleared = console_capture_->Clear(tab_id);

  base::Value::Dict response;
  response.Set("cleared", static_cast<int>(cleared));

  std::string json;
  base::JSONWriter::Write(response, &json);
  std::move(callback).Run(200, "application/json", std::move(json));
}
```

Add the RE2 include at top of `abp_controller.cc`:

```cpp
#include "third_party/re2/src/re2/re2.h"
```

- [ ] **Step 7: Add routing in HandleRequest**

In `abp_controller.cc`, in the `HandleRequest` routing section, add a new route block. Place it after the network route (after line ~2434) and before the next route:

```cpp
  // Route: /api/v1/console
  if (resource == "console") {
    if (segments.size() == 3) {
      if (method == "GET") {
        HandleConsoleQuery(query_string, std::move(callback));
      } else if (method == "DELETE") {
        HandleConsoleClear(query_string, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    SendError(404, "Not found", std::move(callback));
    return;
  }
```

- [ ] **Step 8: Verify build compiles**

Run: `autoninja -C out/Default chrome`

Expected: Compiles with no errors.

- [ ] **Step 9: Commit**

```bash
git add chrome/browser/abp/abp_controller.h \
        chrome/browser/abp/abp_controller.cc
git commit -m "feat(abp): wire console capture into controller with REST API

Add GET /api/v1/console (query) and DELETE /api/v1/console (clear)
endpoints. Per-tab AbpConsoleObserver created in GetOrCreateTabState,
destroyed on tab close. Shared AbpConsoleCapture buffer queried with
level, pattern, tab_id, limit, and after_id filters."
```

---

### Task 5: Add browser_console MCP tool

**Files:**
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:703-726` (tool definition)
- Modify: `chrome/browser/abp/abp_mcp_handler.cc:1070-1075` (tool dispatch)
- Modify: `chrome/browser/abp/abp_mcp_handler.h` (method declaration)

- [ ] **Step 1: Add tool definition in GetToolDefinitions()**

After the `browser_curl` tool definition (after line 726), add:

```cpp
  // 20. browser_console — query JavaScript console messages
  tools.Append(
      ToolBuilder("browser_console")
          .Description(
              "Query JavaScript console messages including logs, errors, "
              "warnings, and browser messages (CORS, CSP). Use to debug "
              "page behavior without requiring an action cycle.\n\n"
              "Returns buffered console messages with optional filters. "
              "Use after_id to poll for new messages since last query.\n\n"
              "Set clear=true to clear the buffer instead of querying.")
          .OptionalStringEnum("level",
              "Minimum severity level",
              {"verbose", "info", "warning", "error"})
          .OptionalString("pattern",
              "RE2 regex filter on message text (case-insensitive)")
          .OptionalString("tab_id",
              "Filter to specific tab ID")
          .OptionalNumber("limit",
              "Maximum entries to return (default 100)")
          .OptionalNumber("after_id",
              "Return only entries with id greater than this value")
          .OptionalBoolean("clear",
              "Clear the buffer instead of querying (uses tab_id if set)")
          .Build());
```

- [ ] **Step 2: Add dispatch in HandleToolsCall()**

In `HandleToolsCall()`, before the `else` block (before line 1073), add:

```cpp
  } else if (*name == "browser_console") {
    CallBrowserConsole(*args, std::move(request_id), std::move(callback));
```

- [ ] **Step 3: Add CallBrowserConsole declaration in header**

In `abp_mcp_handler.h`, add after the `CallBrowserCurl` declaration:

```cpp
  void CallBrowserConsole(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
```

- [ ] **Step 4: Add CallBrowserConsole implementation**

In `abp_mcp_handler.cc`, add after `CallBrowserCurl`:

```cpp
void AbpMcpHandler::CallBrowserConsole(
    const base::Value::Dict& args,
    base::Value request_id,
    ResponseWithHeadersCallback callback) {
  // Check if this is a clear operation.
  if (auto clear = args.FindBool("clear"); clear && *clear) {
    std::string path = "/api/v1/console";
    if (const std::string* tab_id = args.FindString("tab_id")) {
      path += "?tab_id=" +
              base::EscapeQueryParamValue(*tab_id, /*use_plus=*/false);
    }
    controller_->HandleRequest(
        "DELETE", path, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
    return;
  }

  // Build query string from optional filter params.
  std::string path = "/api/v1/console";
  std::vector<std::string> qp;

  if (const std::string* level = args.FindString("level")) {
    qp.push_back("level=" +
                 base::EscapeQueryParamValue(*level, /*use_plus=*/false));
  }
  if (const std::string* pattern = args.FindString("pattern")) {
    qp.push_back("pattern=" +
                 base::EscapeQueryParamValue(*pattern, /*use_plus=*/false));
  }
  if (const std::string* tab_id = args.FindString("tab_id")) {
    qp.push_back("tab_id=" +
                 base::EscapeQueryParamValue(*tab_id, /*use_plus=*/false));
  }
  if (auto limit = args.FindDouble("limit")) {
    qp.push_back("limit=" + base::NumberToString(static_cast<int>(*limit)));
  }
  if (auto after_id = args.FindDouble("after_id")) {
    qp.push_back("after_id=" +
                 base::NumberToString(static_cast<int64_t>(*after_id)));
  }

  if (!qp.empty()) {
    path += "?";
    for (size_t i = 0; i < qp.size(); ++i) {
      if (i > 0) path += "&";
      path += qp[i];
    }
  }

  controller_->HandleRequest(
      "GET", path, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}
```

- [ ] **Step 5: Add browser_console to human-mode allow list**

In `HandleToolsCall()`, find the human-mode read-only tool check (around line 1013). Add `"browser_console"` to the allowed tools list:

```cpp
    if (*name != "browser_get_status" && *name != "browser_screenshot" &&
        *name != "browser_text" && *name != "browser_tabs" &&
        *name != "browser_console") {
```

- [ ] **Step 6: Verify build compiles**

Run: `autoninja -C out/Default chrome`

Expected: Compiles with no errors.

- [ ] **Step 7: Commit**

```bash
git add chrome/browser/abp/abp_mcp_handler.h \
        chrome/browser/abp/abp_mcp_handler.cc
git commit -m "feat(abp): add browser_console MCP tool

Tool 20: browser_console for querying JS console messages with level,
pattern, tab_id, limit, after_id filters. Supports clear=true to reset
the buffer. Allowed in human input mode (read-only)."
```

---

## Chunk 3: Testing

### Task 6: Manual smoke test

- [ ] **Step 1: Build and launch ABP**

```bash
autoninja -C out/Default chrome
./out/Default/ABP.app/Contents/MacOS/ABP --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S) --no-first-run
```

- [ ] **Step 2: Test console.log capture via REST**

```bash
# Create a tab and navigate
TAB=$(curl -s -X POST http://localhost:8222/api/v1/tabs \
  -H "Content-Type: application/json" \
  -d '{"url":"about:blank"}' | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")

# Execute console.log
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/execute" \
  -H "Content-Type: application/json" \
  -d '{"script":"console.log(\"hello from ABP\"); console.warn(\"warning test\"); console.error(\"error test\");"}'

# Query all console messages
curl -s "http://localhost:8222/api/v1/console" | python3 -m json.tool
```

Expected: Response contains 3 entries with levels "info", "warning", "error".

- [ ] **Step 3: Test level filter**

```bash
curl -s "http://localhost:8222/api/v1/console?level=warning" | python3 -m json.tool
```

Expected: Only warning and error entries returned.

- [ ] **Step 4: Test pattern filter**

```bash
curl -s "http://localhost:8222/api/v1/console?pattern=hello" | python3 -m json.tool
```

Expected: Only the "hello from ABP" entry returned.

- [ ] **Step 5: Test after_id pagination**

```bash
# Get first entry's id
FIRST_ID=$(curl -s "http://localhost:8222/api/v1/console?limit=1" | python3 -c "import sys,json; print(json.load(sys.stdin)['entries'][0]['id'])")

# Query after that id
curl -s "http://localhost:8222/api/v1/console?after_id=$FIRST_ID" | python3 -m json.tool
```

Expected: Returns entries after the first one.

- [ ] **Step 6: Test clear**

```bash
curl -s -X DELETE "http://localhost:8222/api/v1/console" | python3 -m json.tool
```

Expected: `{"cleared": 3}` (or however many were in the buffer).

- [ ] **Step 7: Test CORS error capture**

```bash
# Navigate to a page and trigger a CORS error
curl -s -X POST "http://localhost:8222/api/v1/tabs/$TAB/execute" \
  -H "Content-Type: application/json" \
  -d '{"script":"fetch(\"https://httpbin.org/get\").catch(e => {})"}'

# Wait a moment for the error to appear
sleep 2

# Check for CORS error in console
curl -s "http://localhost:8222/api/v1/console?level=error" | python3 -m json.tool
```

Expected: CORS-related error message appears in the buffer.

- [ ] **Step 8: Test MCP tool**

```bash
# Initialize MCP
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"test","version":"1.0"},"capabilities":{}}}'

# List tools — verify browser_console appears
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' | python3 -c "import sys,json; tools=[t['name'] for t in json.load(sys.stdin)['result']['tools']]; print('browser_console' in tools)"

# Call browser_console tool
curl -s -X POST http://localhost:8222/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"browser_console","arguments":{"level":"error"}}}'
```

Expected: `True` for tool presence check, console entries in tool response.

- [ ] **Step 9: Test invalid regex returns 400**

```bash
curl -s -w "\n%{http_code}" "http://localhost:8222/api/v1/console?pattern=[invalid"
```

Expected: HTTP 400 with error message about invalid regex.

---

### Task 7: Update API docs and MCP skill

**Files:**
- Modify: `CLAUDE.md` (add console endpoints to API table)
- Modify: `tools/abp-npm/skills/abp-browser/SKILL.md` (add browser_console tool docs if applicable)

- [ ] **Step 1: Add console endpoints to CLAUDE.md API table**

In the API reference table in CLAUDE.md, add after the Network section:

```markdown
| **Console** | | |
| GET | `/api/v1/console` | Query buffered console messages |
| DELETE | `/api/v1/console` | Clear console buffer |
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(abp): add console capture endpoints to API reference"
```
