#include "chrome/browser/abp/abp_controller.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "base/base64.h"
#include "chrome/browser/abp/abp_action_context.h"
#include "chrome/browser/abp/abp_input_dispatcher.h"
#include "base/command_line.h"
#include "chrome/browser/abp/abp_switches.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_download_observer.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/webp_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace abp {

// KeyInfo implementation
KeyInfo::KeyInfo() = default;
KeyInfo::KeyInfo(const std::string& k,
                 const std::string& c,
                 int wvk,
                 int nvk,
                 bool is_mod,
                 int mod_flag)
    : key(k),
      code(c),
      windows_virtual_key(wvk),
      native_virtual_key(nvk),
      is_modifier(is_mod),
      modifier_flag(mod_flag) {}
KeyInfo::~KeyInfo() = default;
KeyInfo::KeyInfo(const KeyInfo&) = default;
KeyInfo& KeyInfo::operator=(const KeyInfo&) = default;

// HeldKeyState implementation
AbpController::HeldKeyState::HeldKeyState() = default;
AbpController::HeldKeyState::~HeldKeyState() = default;
AbpController::HeldKeyState::HeldKeyState(const HeldKeyState&) = default;
AbpController::HeldKeyState& AbpController::HeldKeyState::operator=(const HeldKeyState&) = default;

// ActionCompleteWaiter implementation
AbpController::ActionCompleteWaiter::ActionCompleteWaiter() = default;
AbpController::ActionCompleteWaiter::~ActionCompleteWaiter() = default;

// PendingDialog implementation
AbpController::PendingDialog::PendingDialog() = default;
AbpController::PendingDialog::~PendingDialog() = default;
AbpController::PendingDialog::PendingDialog(const PendingDialog&) = default;
AbpController::PendingDialog& AbpController::PendingDialog::operator=(
    const PendingDialog&) = default;

// TabState implementation
AbpController::TabState::TabState() = default;
AbpController::TabState::~TabState() = default;
AbpController::TabState::TabState(TabState&&) = default;
AbpController::TabState& AbpController::TabState::operator=(TabState&&) = default;

bool AbpController::TabState::IsIdle() const {
  return !cdp_client && !cursor.active && !execution.debugger_enabled &&
         held_keys.held_keys.empty() && !pending_dialog.has_value() &&
         !action_waiter;
}

void AbpController::TabState::Reset() {
  cdp_client.reset();
  cursor = VirtualCursorState{};
  execution = ExecutionState{};
  held_keys = HeldKeyState{};
  pending_dialog.reset();
  action_waiter.reset();
}

AbpController::TabState& AbpController::GetOrCreateTabState(
    const std::string& tab_id) {
  return tab_states_[tab_id];
}

void AbpController::CleanupTabState(const std::string& tab_id) {
  tab_states_.erase(tab_id);
}

namespace {

// Key code mapping table for CDP Input.dispatchKeyEvent
// See: https://chromedevtools.github.io/devtools-protocol/tot/Input/#method-dispatchKeyEvent
struct KeyMapping {
  const char* name;
  const char* key;
  const char* code;
  int vk;
  bool is_modifier;
  int modifier_flag;  // 1=Alt, 2=Ctrl, 4=Meta, 8=Shift
};

// clang-format off
constexpr KeyMapping kKeyMappings[] = {
    // Modifiers
    {"Alt", "Alt", "AltLeft", 18, true, 1},
    {"AltLeft", "Alt", "AltLeft", 18, true, 1},
    {"AltRight", "Alt", "AltRight", 18, true, 1},
    {"Control", "Control", "ControlLeft", 17, true, 2},
    {"ControlLeft", "Control", "ControlLeft", 17, true, 2},
    {"ControlRight", "Control", "ControlRight", 17, true, 2},
    {"Meta", "Meta", "MetaLeft", 91, true, 4},
    {"MetaLeft", "Meta", "MetaLeft", 91, true, 4},
    {"MetaRight", "Meta", "MetaRight", 92, true, 4},
    {"Shift", "Shift", "ShiftLeft", 16, true, 8},
    {"ShiftLeft", "Shift", "ShiftLeft", 16, true, 8},
    {"ShiftRight", "Shift", "ShiftRight", 16, true, 8},

    // Special keys
    {"Enter", "Enter", "Enter", 13, false, 0},
    {"Tab", "Tab", "Tab", 9, false, 0},
    {"Escape", "Escape", "Escape", 27, false, 0},
    {"Backspace", "Backspace", "Backspace", 8, false, 0},
    {"Delete", "Delete", "Delete", 46, false, 0},
    {"Insert", "Insert", "Insert", 45, false, 0},
    {"Home", "Home", "Home", 36, false, 0},
    {"End", "End", "End", 35, false, 0},
    {"PageUp", "PageUp", "PageUp", 33, false, 0},
    {"PageDown", "PageDown", "PageDown", 34, false, 0},
    {"Space", " ", "Space", 32, false, 0},
    {" ", " ", "Space", 32, false, 0},

    // Arrow keys
    {"ArrowUp", "ArrowUp", "ArrowUp", 38, false, 0},
    {"ArrowDown", "ArrowDown", "ArrowDown", 40, false, 0},
    {"ArrowLeft", "ArrowLeft", "ArrowLeft", 37, false, 0},
    {"ArrowRight", "ArrowRight", "ArrowRight", 39, false, 0},

    // Function keys
    {"F1", "F1", "F1", 112, false, 0},
    {"F2", "F2", "F2", 113, false, 0},
    {"F3", "F3", "F3", 114, false, 0},
    {"F4", "F4", "F4", 115, false, 0},
    {"F5", "F5", "F5", 116, false, 0},
    {"F6", "F6", "F6", 117, false, 0},
    {"F7", "F7", "F7", 118, false, 0},
    {"F8", "F8", "F8", 119, false, 0},
    {"F9", "F9", "F9", 120, false, 0},
    {"F10", "F10", "F10", 121, false, 0},
    {"F11", "F11", "F11", 122, false, 0},
    {"F12", "F12", "F12", 123, false, 0},

    // Number row (digits and symbols need special handling)
    {"0", "0", "Digit0", 48, false, 0},
    {"1", "1", "Digit1", 49, false, 0},
    {"2", "2", "Digit2", 50, false, 0},
    {"3", "3", "Digit3", 51, false, 0},
    {"4", "4", "Digit4", 52, false, 0},
    {"5", "5", "Digit5", 53, false, 0},
    {"6", "6", "Digit6", 54, false, 0},
    {"7", "7", "Digit7", 55, false, 0},
    {"8", "8", "Digit8", 56, false, 0},
    {"9", "9", "Digit9", 57, false, 0},
};
// clang-format on

}  // namespace

// Key info helper - defined outside anonymous namespace so it's accessible
KeyInfo GetKeyInfo(const std::string& key_name) {
  // First check the mapping table
  for (const auto& mapping : kKeyMappings) {
    if (key_name == mapping.name) {
      return {mapping.key, mapping.code, mapping.vk, mapping.vk,
              mapping.is_modifier, mapping.modifier_flag};
    }
  }

  // Handle single lowercase letters (a-z)
  if (key_name.length() == 1) {
    char c = key_name[0];
    if (c >= 'a' && c <= 'z') {
      std::string code = std::string("Key") + static_cast<char>(std::toupper(c));
      int vk = std::toupper(c);  // VK codes for letters are uppercase ASCII
      return {key_name, code, vk, vk, false, 0};
    }
    // Handle uppercase letters
    if (c >= 'A' && c <= 'Z') {
      std::string code = std::string("Key") + c;
      int vk = c;
      return {key_name, code, vk, vk, false, 0};
    }
  }

  // Fallback: use the key name as-is
  LOG(WARNING) << "ABP: Unknown key name: " << key_name << ", using as-is";
  return {key_name, key_name, 0, 0, false, 0};
}

int ModifiersToFlags(const std::vector<std::string>& modifiers) {
  int flags = 0;
  for (const auto& mod : modifiers) {
    if (mod == "Alt" || mod == "AltLeft" || mod == "AltRight") {
      flags |= 1;
    } else if (mod == "Control" || mod == "ControlLeft" || mod == "ControlRight") {
      flags |= 2;
    } else if (mod == "Meta" || mod == "MetaLeft" || mod == "MetaRight") {
      flags |= 4;
    } else if (mod == "Shift" || mod == "ShiftLeft" || mod == "ShiftRight") {
      flags |= 8;
    }
  }
  return flags;
}

namespace {

// Parse path like "/api/v1/tabs/ABC123/navigate" into segments
std::vector<std::string> ParsePath(const std::string& path) {
  std::vector<std::string> segments;
  for (const auto& segment : base::SplitString(
           path, "/", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    segments.push_back(segment);
  }
  return segments;
}

// Process screenshot bitmap and save to file (runs on background thread)
// Note: Cursor rendering is handled by the virtual cursor overlay layer
// (InspectorCursorDrawer) which is captured automatically via CopyFromSurface.
void ProcessAndSaveScreenshot(
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    SkBitmap bitmap) {
  // This runs on a background thread

  // Use the bitmap directly - it was already copied when passed here
  SkBitmap output_bitmap = std::move(bitmap);

  // Encode as WebP
  std::optional<std::vector<uint8_t>> encoded =
      gfx::WebpCodec::Encode(output_bitmap, 80);

  if (!encoded || encoded->empty()) {
    LOG(WARNING) << "ABP: Failed to encode screenshot as WebP";
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::string()));
    return;
  }

  // Create parent directory if needed
  base::FilePath dir = screenshot_path.DirName();
  if (!base::DirectoryExists(dir)) {
    base::CreateDirectory(dir);
  }

  // Write to file
  bool success = base::WriteFile(screenshot_path, *encoded);

  if (success) {
    LOG(INFO) << "ABP: Saved screenshot to " << screenshot_path.value();
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), screenshot_path.AsUTF8Unsafe()));
  } else {
    LOG(WARNING) << "ABP: Failed to write screenshot to "
                 << screenshot_path.value();
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::string()));
  }
}

}  // namespace

// AbpCdpClient implementation

AbpCdpClient::AbpCdpClient(scoped_refptr<content::DevToolsAgentHost> host)
    : host_(std::move(host)) {
  host_->AttachClient(this);
}

AbpCdpClient::~AbpCdpClient() {
  if (host_) {
    host_->DetachClient(this);
  }
}

void AbpCdpClient::SendCommand(const std::string& method,
                               const base::Value::Dict& params,
                               CdpCallback callback) {
  int command_id = next_command_id_++;
  pending_callbacks_[command_id] = std::move(callback);

  // Build CDP message: {"id": N, "method": "...", "params": {...}}
  base::Value::Dict message;
  message.Set("id", command_id);
  message.Set("method", method);
  message.Set("params", params.Clone());

  std::string json;
  base::JSONWriter::Write(message, &json);

  // Send to DevTools agent
  host_->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void AbpCdpClient::SetEventListener(EventCallback callback) {
  event_listener_ = std::move(callback);
}

void AbpCdpClient::ClearEventListener() {
  event_listener_.Reset();
}

void AbpCdpClient::DispatchProtocolMessage(content::DevToolsAgentHost* host,
                                           base::span<const uint8_t> message) {
  // Parse the CDP response
  std::string json(message.begin(), message.end());
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;
  }

  const base::Value::Dict& response = parsed->GetDict();

  // Check for command response (has "id")
  auto id = response.FindInt("id");
  if (!id) {
    // This is an event notification
    const std::string* method = response.FindString("method");
    if (method && event_listener_) {
      const base::Value::Dict* params = response.FindDict("params");
      if (params) {
        event_listener_.Run(*method, *params);
      } else {
        base::Value::Dict empty_params;
        event_listener_.Run(*method, empty_params);
      }
    }
    return;
  }

  auto it = pending_callbacks_.find(*id);
  if (it == pending_callbacks_.end()) {
    return;
  }

  CdpCallback callback = std::move(it->second);
  pending_callbacks_.erase(it);

  // Check for error
  const base::Value::Dict* error = response.FindDict("error");
  if (error) {
    const std::string* error_msg = error->FindString("message");
    std::move(callback).Run(false, error_msg ? *error_msg : "Unknown error");
    return;
  }

  // Get result
  const base::Value* result = response.Find("result");
  if (result) {
    std::string result_json;
    base::JSONWriter::Write(*result, &result_json);
    std::move(callback).Run(true, result_json);
  } else {
    std::move(callback).Run(true, "{}");
  }
}

void AbpCdpClient::AgentHostClosed(content::DevToolsAgentHost* host) {
  host_ = nullptr;
  // Fail all pending callbacks
  for (auto& [id, callback] : pending_callbacks_) {
    std::move(callback).Run(false, "Agent host closed");
  }
  pending_callbacks_.clear();
}

// ScreenshotOptions implementation

AbpController::ScreenshotOptions::ScreenshotOptions() = default;
AbpController::ScreenshotOptions::~ScreenshotOptions() = default;
AbpController::ScreenshotOptions::ScreenshotOptions(const ScreenshotOptions&) = default;
AbpController::ScreenshotOptions& AbpController::ScreenshotOptions::operator=(
    const ScreenshotOptions&) = default;

// AbpController implementation

AbpController::AbpController()
    : event_collector_(std::make_unique<AbpEventCollector>(this)),
      input_dispatcher_(std::make_unique<AbpInputDispatcher>(this)) {}

AbpController::~AbpController() = default;

void AbpController::SetHistoryController(
    AbpHistoryController* history_controller) {
  history_controller_ = history_controller;
}

std::string AbpController::GetActiveTabId() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Get the first browser with an active tab
  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc) {
        // Use GetOrCreateFor() to match FindWebContents() which also uses it.
        // Note: GetOrCreateForTab() returns a 'tab' target with a different ID.
        scoped_refptr<content::DevToolsAgentHost> host =
            content::DevToolsAgentHost::GetOrCreateFor(wc);
        if (host) {
          return host->GetId();
        }
      }
    }
  }

  return std::string();
}

void AbpController::CenterCursorInTab(const std::string& tab_id,
                                       base::OnceClosure callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab called for tab " << tab_id;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - WebContents null for tab " << tab_id;
    std::move(callback).Run();
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - RWHV null for tab " << tab_id;
    std::move(callback).Run();
    return;
  }

  // Get viewport size.
  gfx::Size viewport_size = rwhv->GetVisibleViewportSize();
  double center_x = viewport_size.width() / 2.0;
  double center_y = viewport_size.height() / 2.0;

  LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab"
            << " tab=" << tab_id
            << " viewport=" << viewport_size.width() << "x" << viewport_size.height()
            << " center=(" << center_x << ", " << center_y << ")";

  // Update internal virtual cursor state.
  UpdateVirtualCursorState(tab_id, center_x, center_y);

  // Enable and set virtual cursor via Mojo for on-screen rendering.
  // TEMPORARY: Check if RenderWidgetHost is ready before calling Mojo methods
  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (rwh && rwh->GetProcess() && rwh->GetProcess()->IsInitializedAndNotDead()) {
    LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab - calling SetVirtualCursorEnabledViaMojo(true)";
    SetVirtualCursorEnabledViaMojo(wc, true);
    LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab - calling SetVirtualCursorViaMojo(" << center_x << ", " << center_y << ", true)";
    SetVirtualCursorViaMojo(wc, center_x, center_y, true);
  } else {
    VLOG(1) << "ABP DEBUG L1: CenterCursorInTab - skipping Mojo calls, RWH not ready";
  }

  LOG(INFO) << "ABP DEBUG L1: CenterCursorInTab completed";
  std::move(callback).Run();
}

bool AbpController::IsBrowserReady() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      content::RenderWidgetHostView* rwhv = wc ? wc->GetRenderWidgetHostView() : nullptr;
      LOG(INFO) << "ABP DEBUG L1: IsBrowserReady check"
                << " wc=" << (wc ? "valid" : "null")
                << " rwhv=" << (rwhv ? "valid" : "null");
      if (rwhv) {
        return true;
      }
    }
  }
  LOG(INFO) << "ABP DEBUG L1: IsBrowserReady - not ready yet";
  return false;
}

void AbpController::GetBrowserStatus(ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  bool has_browser_window = false;
  bool has_devtools = false;

  const BrowserList* browser_list = BrowserList::GetInstance();
  for (auto it = browser_list->begin(); it != browser_list->end(); ++it) {
    Browser* browser = *it;
    if (browser->tab_strip_model()->count() > 0) {
      has_browser_window = true;
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc) {
        // Check if we can create/get a CDP client (indicates DevTools ready)
        AbpCdpClient* client = GetOrCreateCdpClient(wc);
        if (client) {
          has_devtools = true;
        }
      }
      break;
    }
  }

  bool ready = has_browser_window && has_devtools;

  base::Value::Dict components;
  components.Set("http_server", true);  // Always true if handling request
  components.Set("browser_window", has_browser_window);
  components.Set("devtools", has_devtools);

  base::Value::Dict data;
  data.Set("ready", ready);
  data.Set("state", ready ? "ready" : "initializing");
  data.Set("components", std::move(components));

  if (!ready) {
    if (!has_browser_window) {
      data.Set("message", "Waiting for browser window");
    } else if (!has_devtools) {
      data.Set("message", "Waiting for DevTools connection");
    }
  }

  base::Value::Dict response;
  response.Set("success", true);
  response.Set("data", std::move(data));

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::CaptureScreenshotForHistory(
    const std::string& tab_id,
    int64_t timestamp,
    bool is_before,
    base::OnceCallback<void(std::string path)> callback) {
  VLOG(1) << "ABP: CaptureScreenshotForHistory is_before=" << is_before
               << " history_controller=" << (history_controller_ ? "yes" : "no")
               << " screenshots_enabled="
               << (history_controller_ ? (history_controller_->ScreenshotsEnabled() ? "yes" : "no") : "n/a");
  if (!history_controller_ || !history_controller_->ScreenshotsEnabled()) {
    VLOG(1) << "ABP: CaptureScreenshotForHistory - skipping (disabled)";
    std::move(callback).Run("");
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run("");
    return;
  }

  base::FilePath screenshot_path =
      history_controller_->GetScreenshotPath(tab_id, timestamp, is_before);

  // Use direct C++ capture with CopyFromSurface
  // Note: Cursor is rendered by virtual cursor overlay and captured automatically
  CaptureScreenshotDirect(wc, screenshot_path, std::move(callback));
}

void AbpController::CaptureScreenshotDirect(
    content::WebContents* web_contents,
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!web_contents) {
    std::move(callback).Run("");
    return;
  }

  content::RenderWidgetHostView* rwhv =
      web_contents->GetRenderWidgetHostView();
  if (!rwhv) {
    VLOG(1) << "ABP: No RenderWidgetHostView for screenshot";
    std::move(callback).Run("");
    return;
  }

  // Capture the surface directly with 5 second timeout
  // Note: The virtual cursor is rendered by InspectorCursorDrawer in the
  // inspector overlay layer, which is included automatically in CopyFromSurface.
  VLOG(1) << "ABP: CaptureScreenshotDirect - calling CopyFromSurface";
  rwhv->CopyFromSurface(
      gfx::Rect(),   // empty = full viewport
      gfx::Size(),   // empty = native resolution
      base::Seconds(5),  // timeout
      base::BindOnce(&AbpController::OnSurfaceCopied,
                     weak_factory_.GetWeakPtr(),
                     screenshot_path,
                     std::move(callback)));
}

void AbpController::OnSurfaceCopied(
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    const content::CopyFromSurfaceResult& result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  VLOG(1) << "ABP: OnSurfaceCopied - callback received";

  if (!result.has_value()) {
    VLOG(1) << "ABP: CopyFromSurface failed: " << result.error();
    std::move(callback).Run("");
    return;
  }

  const SkBitmap& bitmap = result->bitmap;
  if (bitmap.empty()) {
    VLOG(1) << "ABP: CopyFromSurface returned empty bitmap";
    std::move(callback).Run("");
    return;
  }

  // Make a deep copy to pass to the background thread
  SkBitmap bitmap_copy;
  bitmap_copy.allocPixels(bitmap.info());
  bitmap.readPixels(bitmap_copy.info(), bitmap_copy.getPixels(),
                    bitmap_copy.rowBytes(), 0, 0);

  // Process and save on a background thread (using free function in anon namespace)
  // Note: Cursor is already in the bitmap from the virtual cursor overlay layer
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::TaskPriority::USER_VISIBLE, base::MayBlock()},
      base::BindOnce(&ProcessAndSaveScreenshot,
                     screenshot_path,
                     std::move(callback),
                     std::move(bitmap_copy)));
}

void AbpController::OnHistoryMarkupInjected(
    const std::string& tab_id,
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    bool success,
    const std::string& result) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run("");
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(callback).Run("");
    return;
  }

  // Take screenshot via CDP
  base::Value::Dict cdp_params;
  cdp_params.Set("format", "webp");
  cdp_params.Set("quality", 80);

  client->SendCommand(
      "Page.captureScreenshot", cdp_params,
      base::BindOnce(&AbpController::OnHistoryScreenshotCaptured,
                     weak_factory_.GetWeakPtr(), tab_id, screenshot_path,
                     std::move(callback)));
}

void AbpController::OnHistoryScreenshotCaptured(
    const std::string& tab_id,
    const base::FilePath& screenshot_path,
    base::OnceCallback<void(std::string path)> callback,
    bool success,
    const std::string& result) {
  // Clean up injected styles
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc) {
    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (client) {
      std::string cleanup_script = R"(
        (function() {
          const style = document.getElementById('abp-history-style');
          if (style) style.remove();
          const cursor = document.getElementById('abp-cursor-indicator');
          if (cursor) cursor.remove();
          return true;
        })()
      )";
      base::Value::Dict cleanup_params;
      cleanup_params.Set("expression", cleanup_script);
      client->SendCommand("Runtime.evaluate", cleanup_params,
                          base::DoNothing());
    }
  }

  if (!success) {
    std::move(callback).Run("");
    return;
  }

  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    std::move(callback).Run("");
    return;
  }

  const std::string* data = parsed->GetDict().FindString("data");
  if (!data) {
    std::move(callback).Run("");
    return;
  }

  // Decode and write file
  std::optional<std::vector<uint8_t>> decoded = base::Base64Decode(*data);
  if (!decoded) {
    std::move(callback).Run("");
    return;
  }

  // Write to file on ThreadPool
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath p, std::vector<uint8_t> content) -> std::string {
            if (base::WriteFile(p, content)) {
              return p.AsUTF8Unsafe();
            }
            return "";
          },
          screenshot_path, std::move(*decoded)),
      std::move(callback));
}

void AbpController::HandleRequest(const std::string& method,
                                  const std::string& path,
                                  const std::string& body,
                                  ResponseCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Parse JSON body if present
  base::Value::Dict params;
  if (!body.empty()) {
    auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      params = std::move(parsed->GetDict());
    }
  }

  // Parse path: /api/v1/tabs, /api/v1/tabs/{id}, /api/v1/tabs/{id}/action
  std::vector<std::string> segments = ParsePath(path);

  // Validate /api/v1 prefix
  if (segments.size() < 3 || segments[0] != "api" || segments[1] != "v1") {
    SendError(404, "Not found", std::move(callback));
    return;
  }

  const std::string& resource = segments[2];

  // Route: /api/v1/tabs
  if (resource == "tabs") {
    if (segments.size() == 3) {
      // /api/v1/tabs
      if (method == "GET") {
        ListTabs(std::move(callback));
      } else if (method == "POST") {
        CreateTab(params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }

    const std::string& tab_id = segments[3];

    if (segments.size() == 4) {
      // /api/v1/tabs/{id}
      if (method == "GET") {
        GetTab(tab_id, std::move(callback));
      } else if (method == "DELETE") {
        CloseTab(tab_id, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }

    if (segments.size() == 5) {
      const std::string& action = segments[4];

      // /api/v1/tabs/{id}/{action}
      if (method != "POST" && method != "GET") {
        SendError(405, "Method not allowed", std::move(callback));
        return;
      }

      if (action == "navigate") {
        Navigate(tab_id, params, std::move(callback));
      } else if (action == "reload") {
        Reload(tab_id, std::move(callback));
      } else if (action == "back") {
        GoBack(tab_id, std::move(callback));
      } else if (action == "forward") {
        GoForward(tab_id, std::move(callback));
      } else if (action == "screenshot") {
        if (method == "GET") {
          // GET returns binary WebP directly
          // Extract query string for markup option
          size_t query_pos = path.find('?');
          std::string query = (query_pos != std::string::npos)
                                  ? path.substr(query_pos + 1)
                                  : "";
          BinaryScreenshot(tab_id, query, std::move(callback));
        } else {
          // POST returns JSON with base64 data
          Screenshot(tab_id, params, std::move(callback));
        }
      } else if (action == "execute") {
        ExecuteScript(tab_id, params, std::move(callback));
      } else if (action == "text") {
        GetText(tab_id, params, std::move(callback));
      } else if (action == "click") {
        Click(tab_id, params, std::move(callback));
      } else if (action == "type") {
        Type(tab_id, params, std::move(callback));
      } else if (action == "move") {
        Move(tab_id, params, std::move(callback));
      } else if (action == "wait") {
        Wait(tab_id, params, std::move(callback));
      } else if (action == "scroll") {
        Scroll(tab_id, params, std::move(callback));
      } else if (action == "activate") {
        ActivateTab(tab_id, std::move(callback));
      } else if (action == "stop") {
        StopLoading(tab_id, std::move(callback));
      } else if (action == "execution") {
        if (method == "GET") {
          GetExecutionState(tab_id, std::move(callback));
        } else if (method == "POST") {
          SetExecutionState(tab_id, params, std::move(callback));
        } else {
          SendError(405, "Method not allowed", std::move(callback));
        }
      } else if (action == "dialog") {
        // GET /api/v1/tabs/{id}/dialog - check for pending dialog
        if (method == "GET") {
          GetDialog(tab_id, std::move(callback));
        } else {
          SendError(405, "Method not allowed", std::move(callback));
        }
      } else if (action == "keyboard") {
        // Handle /api/v1/tabs/{id}/keyboard/{sub_action}
        SendError(400, "Missing keyboard sub-action (press, down, up)",
                  std::move(callback));
      } else {
        SendError(404, "Unknown action: " + action, std::move(callback));
      }
      return;
    }

    // Handle 6-segment paths: /api/v1/tabs/{id}/{action}/{sub_action}
    if (segments.size() == 6) {
      const std::string& action = segments[4];
      const std::string& sub_action = segments[5];

      if (method != "POST") {
        SendError(405, "Method not allowed", std::move(callback));
        return;
      }

      if (action == "keyboard") {
        if (sub_action == "press") {
          KeyPress(tab_id, params, std::move(callback));
        } else if (sub_action == "down") {
          KeyDown(tab_id, params, std::move(callback));
        } else if (sub_action == "up") {
          KeyUp(tab_id, params, std::move(callback));
        } else {
          SendError(404, "Unknown keyboard action: " + sub_action,
                    std::move(callback));
        }
      } else if (action == "dialog") {
        // POST /api/v1/tabs/{id}/dialog/accept or /dialog/dismiss
        if (sub_action == "accept") {
          AcceptDialog(tab_id, params, std::move(callback));
        } else if (sub_action == "dismiss") {
          DismissDialog(tab_id, std::move(callback));
        } else {
          SendError(404, "Unknown dialog action: " + sub_action,
                    std::move(callback));
        }
      } else if (action == "mouse") {
        if (sub_action == "scroll") {
          Scroll(tab_id, params, std::move(callback));
        } else {
          SendError(404, "Unknown mouse action: " + sub_action,
                    std::move(callback));
        }
      } else {
        SendError(404, "Unknown action: " + action + "/" + sub_action,
                  std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/browser
  if (resource == "browser") {
    if (segments.size() == 4 && segments[3] == "status") {
      // /api/v1/browser/status
      if (method == "GET") {
        GetBrowserStatus(std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4 && segments[3] == "shutdown") {
      // POST /api/v1/browser/shutdown
      if (method == "POST") {
        ShutdownBrowser(params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/file-chooser/{id}
  if (resource == "file-chooser") {
    if (segments.size() == 4) {
      const std::string& chooser_id = segments[3];
      if (method == "POST") {
        HandleFileChooser(chooser_id, params, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  // Route: /api/v1/downloads
  if (resource == "downloads") {
    if (segments.size() == 3) {
      // GET /api/v1/downloads
      if (method == "GET") {
        // Extract query string
        size_t query_pos = path.find('?');
        std::string query = (query_pos != std::string::npos)
                                ? path.substr(query_pos + 1)
                                : "";
        ListDownloads(query, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 4) {
      const std::string& download_id = segments[3];
      // GET /api/v1/downloads/{id}
      if (method == "GET") {
        GetDownloadStatus(download_id, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
    if (segments.size() == 5 && segments[4] == "cancel") {
      // POST /api/v1/downloads/{id}/cancel
      const std::string& download_id = segments[3];
      if (method == "POST") {
        CancelDownload(download_id, std::move(callback));
      } else {
        SendError(405, "Method not allowed", std::move(callback));
      }
      return;
    }
  }

  SendError(404, "Not found", std::move(callback));
}

void AbpController::ListTabs(ResponseCallback callback) {
  base::Value::List tabs;

  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);

      base::Value::Dict tab;
      tab.Set("id", host->GetId());
      tab.Set("url", wc->GetVisibleURL().spec());
      tab.Set("title", wc->GetTitle());
      tab.Set("active", tab_strip->active_index() == i);
      tabs.Append(std::move(tab));
    }
  }

  SendJson(200, base::Value(std::move(tabs)), std::move(callback));
}

void AbpController::GetTab(const std::string& tab_id,
                           ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  base::Value::Dict tab;
  tab.Set("id", tab_id);
  tab.Set("url", wc->GetVisibleURL().spec());
  tab.Set("title", wc->GetTitle());
  tab.Set("loading", wc->IsLoading());

  SendJson(200, base::Value(std::move(tab)), std::move(callback));
}

void AbpController::CreateTab(const base::Value::Dict& params,
                              ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Get the first available browser
  Browser* browser = nullptr;
  const BrowserList* browser_list = BrowserList::GetInstance();
  auto it = browser_list->begin();
  if (it != browser_list->end()) {
    browser = *it;
  }
  if (!browser) {
    if (history_controller_) {
      history_controller_->RecordAction("", "create_tab", params, nullptr,
                                        false, "NO_BROWSER", "No active browser",
                                        start_time, 0, "", "");
    }
    SendError(500, "No active browser", std::move(callback));
    return;
  }

  const std::string* url = params.FindString("url");
  GURL gurl = url ? GURL(*url) : GURL("about:blank");

  NavigateParams nav_params(browser, gurl, ui::PAGE_TRANSITION_TYPED);
  nav_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  ::Navigate(&nav_params);

  if (nav_params.navigated_or_inserted_contents) {
    content::WebContents* wc = nav_params.navigated_or_inserted_contents;
    auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);

    base::Value::Dict tab;
    tab.Set("id", host->GetId());
    tab.Set("url", wc->GetVisibleURL().spec());

    // Center the virtual cursor in the new tab.
    CenterCursorInTab(host->GetId(), base::DoNothing());

    // Record successful action
    if (history_controller_) {
      int64_t duration_ms =
          base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
      base::Value result_value(tab.Clone());
      history_controller_->RecordAction(host->GetId(), "create_tab", params,
                                        &result_value, true, "", "", start_time,
                                        duration_ms, "", "");
    }

    SendJson(201, base::Value(std::move(tab)), std::move(callback));
  } else {
    if (history_controller_) {
      history_controller_->RecordAction("", "create_tab", params, nullptr,
                                        false, "CREATE_FAILED",
                                        "Failed to create tab", start_time, 0,
                                        "", "");
    }
    SendError(500, "Failed to create tab", std::move(callback));
  }
}

void AbpController::CloseTab(const std::string& tab_id,
                             ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;
  params.Set("tab_id", tab_id);

  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        // Clean up all per-tab state
        CleanupTabState(tab_id);
        tab_strip->CloseWebContentsAt(i, TabCloseTypes::CLOSE_USER_GESTURE);

        base::Value::Dict result;
        if (history_controller_) {
          int64_t duration_ms =
              base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
          base::Value result_value(result.Clone());
          history_controller_->RecordAction(tab_id, "close_tab", params,
                                            &result_value, true, "", "",
                                            start_time, duration_ms, "", "");
        }

        SendJson(200, base::Value(std::move(result)), std::move(callback));
        return;
      }
    }
  }

  if (history_controller_) {
    history_controller_->RecordAction(tab_id, "close_tab", params, nullptr,
                                      false, "TAB_NOT_FOUND", "Tab not found",
                                      start_time, 0, "", "");
  }
  SendError(404, "Tab not found", std::move(callback));
}

void AbpController::Navigate(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  // Validate params early
  const std::string* url = params.FindString("url");
  if (!url) {
    SendError(400, "Missing 'url' parameter", std::move(callback));
    return;
  }

  GURL gurl(*url);
  if (!gurl.is_valid()) {
    SendError(400, "Invalid URL", std::move(callback));
    return;
  }

  std::string url_copy = gurl.spec();

  // Center cursor after navigation so it's in the viewport center.
  // Use longer min_wait_time (10s) to allow page to fully load.
  AbpActionContext::Options options;
  options.center_cursor_after = true;
  options.min_wait_time = base::Seconds(10);

  AbpActionContext::RunWithOptions(
      this, tab_id, "navigate", params, options,
      // Action callback - performs the actual navigation
      base::BindOnce(
          [](std::string url, AbpActionContext* ctx) {
            content::WebContents* wc = ctx->web_contents();
            if (!wc) {
              ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
              return;
            }

            GURL gurl(url);
            wc->GetController().LoadURL(gurl, content::Referrer(),
                                        ui::PAGE_TRANSITION_TYPED, std::string());

            // Set result and signal action complete
            // Wait will happen via WaitForActionComplete
            base::Value::Dict res;
            res.Set("status", "navigated");
            res.Set("url", url);
            ctx->SetResult(std::move(res));
            ctx->OnActionDispatched();
          },
          std::move(url_copy)),
      std::move(callback));
}

void AbpController::Reload(const std::string& tab_id,
                           ResponseCallback callback) {
  base::Value::Dict params;  // Empty params for reload

  // Center cursor after reload so it's in the viewport center.
  // Use longer min_wait_time (10s) to allow page to fully load.
  AbpActionContext::Options options;
  options.center_cursor_after = true;
  options.min_wait_time = base::Seconds(10);

  AbpActionContext::RunWithOptions(
      this, tab_id, "reload", params, options,
      // Action callback - performs the reload
      base::BindOnce([](AbpActionContext* ctx) {
        content::WebContents* wc = ctx->web_contents();
        if (!wc) {
          ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
          return;
        }

        wc->GetController().Reload(content::ReloadType::NORMAL, false);

        base::Value::Dict res;
        res.Set("status", "reloaded");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::GoBack(const std::string& tab_id,
                           ResponseCallback callback) {
  // Early validation - check if we can go back before starting context
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc && !wc->GetController().CanGoBack()) {
    SendError(400, "Cannot go back", std::move(callback));
    return;
  }

  base::Value::Dict params;  // Empty params for back

  // Center cursor after navigation so it's in the viewport center.
  // Use longer min_wait_time (10s) to allow page to fully load.
  AbpActionContext::Options options;
  options.center_cursor_after = true;
  options.min_wait_time = base::Seconds(10);

  AbpActionContext::RunWithOptions(
      this, tab_id, "back", params, options,
      // Action callback - performs the go back
      base::BindOnce([](AbpActionContext* ctx) {
        content::WebContents* wc = ctx->web_contents();
        if (!wc) {
          ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
          return;
        }

        if (!wc->GetController().CanGoBack()) {
          ctx->OnActionError("CANNOT_GO_BACK", "Cannot go back");
          return;
        }

        wc->GetController().GoBack();

        base::Value::Dict res;
        res.Set("status", "navigated_back");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::GoForward(const std::string& tab_id,
                              ResponseCallback callback) {
  // Early validation - check if we can go forward before starting context
  content::WebContents* wc = FindWebContents(tab_id);
  if (wc && !wc->GetController().CanGoForward()) {
    SendError(400, "Cannot go forward", std::move(callback));
    return;
  }

  base::Value::Dict params;  // Empty params for forward

  // Center cursor after navigation so it's in the viewport center.
  // Use longer min_wait_time (10s) to allow page to fully load.
  AbpActionContext::Options options;
  options.center_cursor_after = true;
  options.min_wait_time = base::Seconds(10);

  AbpActionContext::RunWithOptions(
      this, tab_id, "forward", params, options,
      // Action callback - performs the go forward
      base::BindOnce([](AbpActionContext* ctx) {
        content::WebContents* wc = ctx->web_contents();
        if (!wc) {
          ctx->OnActionError("TAB_NOT_FOUND", "Tab not found");
          return;
        }

        if (!wc->GetController().CanGoForward()) {
          ctx->OnActionError("CANNOT_GO_FORWARD", "Cannot go forward");
          return;
        }

        wc->GetController().GoForward();

        base::Value::Dict res;
        res.Set("status", "navigated_forward");
        ctx->SetResult(std::move(res));
        ctx->OnActionDispatched();
      }),
      std::move(callback));
}

void AbpController::Screenshot(const std::string& tab_id,
                               const base::Value::Dict& params,
                               ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // Parse screenshot options from request params
  ScreenshotOptions options;

  // Get the DevToolsAgentHost ID for cursor state lookup
  // Use GetOrCreateFor to match FindWebContents/GetOrCreateCdpClient
  auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
  std::string host_id = host ? host->GetId() : "";

  // Get screenshot sub-object if present
  const base::Value::Dict* screenshot_params = params.FindDict("screenshot");
  if (screenshot_params) {
    if (const std::string* format = screenshot_params->FindString("format")) {
      if (*format == "png" || *format == "jpeg" || *format == "webp") {
        options.format = *format;
      }
    }
    if (auto quality = screenshot_params->FindInt("quality")) {
      options.quality = std::clamp(*quality, 1, 100);
    }
    if (const std::string* markup = screenshot_params->FindString("markup")) {
      options.markup = *markup;
    }
    if (const std::string* mouse = screenshot_params->FindString("mouse")) {
      options.mouse = *mouse;
    }
    if (auto cursor = screenshot_params->FindBool("cursor")) {
      options.cursor = *cursor;
    }
  }

  // Also check top-level params for backward compatibility
  if (const std::string* format = params.FindString("format")) {
    if (*format == "png" || *format == "jpeg" || *format == "webp") {
      options.format = *format;
    }
  }
  if (auto quality = params.FindInt("quality")) {
    options.quality = std::clamp(*quality, 1, 100);
  }
  if (auto cursor = params.FindBool("cursor")) {
    options.cursor = *cursor;
  }

  // If cursor=false, hide the virtual cursor before capturing
  if (!options.cursor) {
    content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
    if (rwhv) {
      content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
      if (rwh) {
        rwh->SetVirtualCursorVisible(false);
      }
    }
  }

  // If markup is requested, inject CSS-only outline styles (handles thousands of elements)
  if (options.markup != "none") {
    // Pure CSS approach: inject a single <style> element with selectors
    // No per-element JS iteration needed - browser CSS engine handles matching
    // O(1) JavaScript, scales to thousands of elements
    std::string css_rules;
    if (options.markup == "interactive") {
      css_rules = R"(
        a, [role='link'] { outline:2px solid #2196F3!important; outline-offset:-2px!important; }
        button, [role='button'], [onclick], [tabindex]:not([tabindex='-1']) { outline:2px solid #4CAF50!important; outline-offset:-2px!important; }
        input:not([type='hidden']) { outline:2px solid #FF9800!important; outline-offset:-2px!important; }
        select { outline:2px solid #9C27B0!important; outline-offset:-2px!important; }
        textarea, [contenteditable='true'] { outline:2px solid #795548!important; outline-offset:-2px!important; }
      )";
    } else if (options.markup == "clickable") {
      css_rules = R"(
        a, [role='link'] { outline:2px solid #2196F3!important; outline-offset:-2px!important; }
        button, [role='button'], [onclick] { outline:2px solid #4CAF50!important; outline-offset:-2px!important; }
      )";
    } else if (options.markup == "typeable") {
      css_rules = R"(
        input:not([type='hidden']):not([type='checkbox']):not([type='radio']):not([type='submit']):not([type='button']),
        textarea, [contenteditable='true'] { outline:2px solid #FF9800!important; outline-offset:-2px!important; }
      )";
    } else if (options.markup == "inputs") {
      css_rules = R"(
        input:not([type='hidden']) { outline:2px solid #FF9800!important; outline-offset:-2px!important; }
        select { outline:2px solid #9C27B0!important; outline-offset:-2px!important; }
        textarea { outline:2px solid #795548!important; outline-offset:-2px!important; }
      )";
    }

    // Synchronous style injection - no rAF needed since CSS applies immediately
    // and Page.captureScreenshot waits for rendering
    std::string script = R"(
      (function() {
        const old = document.getElementById('abp-markup-style');
        if (old) old.remove();
        const style = document.createElement('style');
        style.id = 'abp-markup-style';
        style.textContent = `)" + css_rules + R"(`;
        document.head.appendChild(style);
        return true;
      })()
    )";

    base::Value::Dict js_params;
    js_params.Set("expression", script);
    js_params.Set("returnByValue", true);

    client->SendCommand(
        "Runtime.evaluate", js_params,
        base::BindOnce(&AbpController::OnMarkupInjected,
                       weak_factory_.GetWeakPtr(), tab_id,
                       std::move(callback), options));
    return;
  }

  // The Mojo virtual cursor is already positioned from previous input actions.
  // CopyFromSurface captures the composited frame which includes the cursor overlay.
  // No CDP Overlay.setVirtualCursor call needed - go directly to screenshot capture.
  CaptureScreenshotWithCursor(tab_id, std::move(callback), options);
}

void AbpController::OnScreenshotResult(ResponseCallback callback,
                                       const ScreenshotOptions& options,
                                       bool success,
                                       const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

  // Parse the result to extract the base64 data
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendError(500, "Invalid CDP response", std::move(callback));
    return;
  }

  const std::string* data = parsed->GetDict().FindString("data");
  if (!data) {
    SendError(500, "No screenshot data", std::move(callback));
    return;
  }

  // Determine MIME type based on format
  std::string mime_type = "image/png";
  if (options.format == "jpeg") {
    mime_type = "image/jpeg";
  } else if (options.format == "webp") {
    mime_type = "image/webp";
  }

  base::Value::Dict response;
  response.Set("data", *data);
  response.Set("mimeType", mime_type);
  response.Set("format", options.format);
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::OnMarkupInjected(const std::string& tab_id,
                                     ResponseCallback callback,
                                     const ScreenshotOptions& options,
                                     bool success,
                                     const std::string& result) {
  // Markup overlay already injected by the JavaScript.
  // The Mojo virtual cursor is already positioned from previous input actions.
  // CopyFromSurface captures the composited frame which includes the cursor overlay.
  CaptureScreenshotWithCursor(tab_id, std::move(callback), options);
}

void AbpController::CaptureScreenshotWithCursor(const std::string& tab_id,
                                                 ResponseCallback callback,
                                                 const ScreenshotOptions& options) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    SendError(500, "No RenderWidgetHostView for screenshot", std::move(callback));
    return;
  }

  // Use CopyFromSurface which includes the inspector overlay (with cursor)
  rwhv->CopyFromSurface(
      gfx::Rect(),   // empty = full viewport
      gfx::Size(),   // empty = native resolution
      base::Seconds(5),  // timeout
      base::BindOnce(&AbpController::OnCursorScreenshotCaptured,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(callback), options));
}

void AbpController::OnCursorScreenshotCaptured(const std::string& tab_id,
                                                ResponseCallback callback,
                                                const ScreenshotOptions& options,
                                                const content::CopyFromSurfaceResult& result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // If CopyFromSurface failed, fall back to CDP Page.captureScreenshot
  // (the cursor won't be visible but the screenshot will work)
  if (!result.has_value() || result->bitmap.empty()) {
    VLOG(1) << "ABP: CopyFromSurface failed (empty=" << (result.has_value() ? result->bitmap.empty() : true) << "), falling back to CDP screenshot";

    content::WebContents* wc = FindWebContents(tab_id);
    if (!wc) {
      SendError(404, "Tab not found", std::move(callback));
      return;
    }

    AbpCdpClient* client = GetOrCreateCdpClient(wc);
    if (!client) {
      SendError(500, "Failed to create CDP client", std::move(callback));
      return;
    }

    // Fall back to CDP Page.captureScreenshot
    base::Value::Dict cdp_params;
    cdp_params.Set("format", options.format);
    if (options.format != "png") {
      cdp_params.Set("quality", options.quality);
    }

    client->SendCommand(
        "Page.captureScreenshot", cdp_params,
        base::BindOnce(&AbpController::OnScreenshotResult,
                       weak_factory_.GetWeakPtr(), std::move(callback), options));
    return;
  }

  const SkBitmap& bitmap = result->bitmap;

  // Encode the bitmap in the requested format
  std::optional<std::vector<uint8_t>> encoded;
  std::string mime_type;

  if (options.format == "png") {
    encoded = gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
    mime_type = "image/png";
  } else if (options.format == "jpeg") {
    encoded = gfx::JPEGCodec::Encode(bitmap, options.quality);
    mime_type = "image/jpeg";
  } else {  // webp
    encoded = gfx::WebpCodec::Encode(bitmap, options.quality);
    mime_type = "image/webp";
  }

  if (!encoded || encoded->empty()) {
    SendError(500, "Failed to encode screenshot", std::move(callback));
    return;
  }

  // Base64 encode the image data
  std::string base64_data = base::Base64Encode(*encoded);

  // Clean up markup style if it was injected
  if (options.markup != "none") {
    content::WebContents* wc = FindWebContents(tab_id);
    if (wc) {
      AbpCdpClient* client = GetOrCreateCdpClient(wc);
      if (client) {
        base::Value::Dict cleanup_params;
        cleanup_params.Set("expression",
            "document.getElementById('abp-markup-style')?.remove()");
        cleanup_params.Set("returnByValue", true);
        client->SendCommand("Runtime.evaluate", cleanup_params,
                            base::BindOnce([](bool, const std::string&) {}));
      }
    }
  }

  base::Value::Dict response;
  response.Set("data", base64_data);
  response.Set("mimeType", mime_type);
  response.Set("format", options.format);
  if (options.markup != "none") {
    response.Set("markup", options.markup);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::ExecuteScript(const std::string& tab_id,
                                  const base::Value::Dict& params,
                                  ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  const std::string* script = params.FindString("script");
  if (!script) {
    SendError(400, "Missing 'script' parameter", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // CDP: Runtime.evaluate
  base::Value::Dict cdp_params;
  cdp_params.Set("expression", *script);
  cdp_params.Set("returnByValue", true);

  client->SendCommand(
      "Runtime.evaluate", cdp_params,
      base::BindOnce(&AbpController::OnExecuteScriptResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void AbpController::OnExecuteScriptResult(ResponseCallback callback,
                                          bool success,
                                          const std::string& result) {
  if (!success) {
    SendError(500, result, std::move(callback));
    return;
  }

  // Parse the result
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendError(500, "Invalid CDP response", std::move(callback));
    return;
  }

  const base::Value::Dict& dict = parsed->GetDict();

  // Check for exception
  const base::Value::Dict* exception = dict.FindDict("exceptionDetails");
  if (exception) {
    const base::Value::Dict* exc = exception->FindDict("exception");
    const std::string* desc = exc ? exc->FindString("description") : nullptr;
    SendError(400, desc ? *desc : "Script exception", std::move(callback));
    return;
  }

  // Get result value
  const base::Value::Dict* cdp_result = dict.FindDict("result");
  if (cdp_result) {
    base::Value::Dict response;
    response.Set("result", cdp_result->Clone());
    SendJson(200, base::Value(std::move(response)), std::move(callback));
  } else {
    SendJson(200, base::Value(base::Value::Dict()), std::move(callback));
  }
}

void AbpController::GetText(const std::string& tab_id,
                            const base::Value::Dict& params,
                            ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to create CDP client", std::move(callback));
    return;
  }

  // Build JS expression based on whether a selector is provided
  std::string expression;
  const std::string* selector = params.FindString("selector");
  if (selector && !selector->empty()) {
    // Escape the selector for embedding in JS string
    std::string escaped_selector = *selector;
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\\", "\\\\");
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "'", "\\'");
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\n", "\\n");
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\r", "\\r");
    expression = "(function() { var el = document.querySelector('" +
                 escaped_selector +
                 "'); return el ? el.innerText : null; })()";
  } else {
    expression = "document.body ? document.body.innerText : ''";
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("expression", expression);
  cdp_params.Set("returnByValue", true);

  client->SendCommand(
      "Runtime.evaluate", cdp_params,
      base::BindOnce(
          [](ResponseCallback cb, base::WeakPtr<AbpController> controller,
             bool success, const std::string& result) {
            if (!controller) return;
            if (!success) {
              controller->SendError(500, result, std::move(cb));
              return;
            }

            auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
            if (!parsed || !parsed->is_dict()) {
              controller->SendError(500, "Invalid CDP response", std::move(cb));
              return;
            }

            const base::Value::Dict& dict = parsed->GetDict();

            // Check for exception
            const base::Value::Dict* exception = dict.FindDict("exceptionDetails");
            if (exception) {
              const base::Value::Dict* exc = exception->FindDict("exception");
              const std::string* desc =
                  exc ? exc->FindString("description") : nullptr;
              controller->SendError(
                  400, desc ? *desc : "Script exception", std::move(cb));
              return;
            }

            // Get result value
            const base::Value::Dict* cdp_result = dict.FindDict("result");
            base::Value::Dict response;
            if (cdp_result) {
              const std::string* text = cdp_result->FindString("value");
              if (text) {
                response.Set("text", *text);
              } else {
                // Value might be null (selector not found)
                response.Set("text", base::Value());
              }
            } else {
              response.Set("text", "");
            }

            controller->SendJson(200, base::Value(std::move(response)),
                                 std::move(cb));
          },
          std::move(callback), weak_factory_.GetWeakPtr()));
}

void AbpController::Click(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  input_dispatcher_->Click(tab_id, params, std::move(callback));
}

void AbpController::Type(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  input_dispatcher_->Type(tab_id, params, std::move(callback));
}

void AbpController::Move(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  input_dispatcher_->Move(tab_id, params, std::move(callback));
}

void AbpController::Wait(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback) {
  // Validate params early
  auto ms_opt = params.FindInt("ms");
  if (!ms_opt) {
    SendError(400, "Missing 'ms' parameter", std::move(callback));
    return;
  }

  int wait_ms = *ms_opt;
  if (wait_ms < 0) {
    SendError(400, "Wait time must be non-negative", std::move(callback));
    return;
  }
  if (wait_ms > 60000) {
    SendError(400, "Wait time must be <= 60000ms", std::move(callback));
    return;
  }

  // Use AbpActionContext with default options (resume + pause enabled)
  // This allows JavaScript to run during the wait period (for animations, etc.)
  // Flow: Resume V8 -> Wait -> Pause V8 -> Screenshot
  AbpActionContext::Run(
      this, tab_id, "wait", params,
      // Action callback - performs the wait
      base::BindOnce(
          [](int ms, AbpActionContext* ctx) {
            // Take a scoped_refptr to keep context alive through async call
            scoped_refptr<AbpActionContext> ctx_ref(ctx);

            // Schedule the completion after the wait time
            content::GetUIThreadTaskRunner({})->PostDelayedTask(
                FROM_HERE,
                base::BindOnce(
                    [](scoped_refptr<AbpActionContext> action_ctx, int waited_ms) {
                      base::Value::Dict res;
                      res.Set("status", "waited");
                      res.Set("ms", waited_ms);
                      action_ctx->SetResult(std::move(res));
                      action_ctx->OnActionDispatched();
                    },
                    ctx_ref, ms),
                base::Milliseconds(ms));
          },
          wait_ms),
      std::move(callback));
}

void AbpController::Scroll(const std::string& tab_id,
                           const base::Value::Dict& params,
                           ResponseCallback callback) {
  input_dispatcher_->Scroll(tab_id, params, std::move(callback));
}

void AbpController::KeyPress(const std::string& tab_id,
                             const base::Value::Dict& params,
                             ResponseCallback callback) {
  input_dispatcher_->KeyPress(tab_id, params, std::move(callback));
}

void AbpController::KeyDown(const std::string& tab_id,
                            const base::Value::Dict& params,
                            ResponseCallback callback) {
  input_dispatcher_->KeyDown(tab_id, params, std::move(callback));
}

void AbpController::KeyUp(const std::string& tab_id,
                          const base::Value::Dict& params,
                          ResponseCallback callback) {
  input_dispatcher_->KeyUp(tab_id, params, std::move(callback));
}

void AbpController::ActivateTab(const std::string& tab_id,
                                ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;
  params.Set("tab_id", tab_id);

  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        // Activate the tab
        tab_strip->ActivateTabAt(i);

        // Also bring the browser window to front
        browser->window()->Activate();

        base::Value::Dict result;
        result.Set("status", "activated");
        result.Set("tab_id", tab_id);
        result.Set("index", i);

        if (history_controller_) {
          int64_t duration_ms =
              base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
          base::Value result_value(result.Clone());
          history_controller_->RecordAction(tab_id, "activate_tab", params,
                                            &result_value, true, "", "",
                                            start_time, duration_ms, "", "");
        }

        SendJson(200, base::Value(std::move(result)), std::move(callback));
        return;
      }
    }
  }

  if (history_controller_) {
    history_controller_->RecordAction(tab_id, "activate_tab", params, nullptr,
                                      false, "TAB_NOT_FOUND", "Tab not found",
                                      start_time, 0, "", "");
  }
  SendError(404, "Tab not found", std::move(callback));
}

void AbpController::StopLoading(const std::string& tab_id,
                                ResponseCallback callback) {
  int64_t start_time = base::Time::Now().InMillisecondsSinceUnixEpoch();
  base::Value::Dict params;
  params.Set("tab_id", tab_id);

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    if (history_controller_) {
      history_controller_->RecordAction(tab_id, "stop_loading", params, nullptr,
                                        false, "TAB_NOT_FOUND", "Tab not found",
                                        start_time, 0, "", "");
    }
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  // Stop loading
  wc->Stop();

  base::Value::Dict result;
  result.Set("status", "stopped");
  result.Set("tab_id", tab_id);

  if (history_controller_) {
    int64_t duration_ms =
        base::Time::Now().InMillisecondsSinceUnixEpoch() - start_time;
    base::Value result_value(result.Clone());
    history_controller_->RecordAction(tab_id, "stop_loading", params,
                                      &result_value, true, "", "",
                                      start_time, duration_ms, "", "");
  }

  SendJson(200, base::Value(std::move(result)), std::move(callback));
}

content::WebContents* AbpController::FindWebContents(
    const std::string& tab_id) {
  for (Browser* browser : *BrowserList::GetInstance()) {
    TabStripModel* tab_strip = browser->tab_strip_model();
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
      if (host->GetId() == tab_id) {
        return wc;
      }
    }
  }
  return nullptr;
}

AbpCdpClient* AbpController::GetOrCreateCdpClient(content::WebContents* wc) {
  auto host = content::DevToolsAgentHost::GetOrCreateFor(wc);
  const std::string& id = host->GetId();

  TabState& state = GetOrCreateTabState(id);
  if (state.cdp_client) {
    return state.cdp_client.get();
  }

  auto client = std::make_unique<AbpCdpClient>(host);
  AbpCdpClient* raw_ptr = client.get();

  // Set up event listener to route CDP events to the event collector,
  // handle dialogs, and forward to action-complete wait logic if active.
  std::string tab_id = id;
  raw_ptr->SetEventListener(base::BindRepeating(
      [](base::WeakPtr<AbpController> controller, std::string tab,
         const std::string& method, const base::Value::Dict& params) {
        if (!controller) {
          return;
        }
        // Route to event collector
        if (controller->event_collector_) {
          controller->event_collector_->OnCdpEvent(tab, method, params);
        }
        // Handle dialog events for pending_dialog tracking
        if (method == "Page.javascriptDialogOpening") {
          const std::string* type = params.FindString("type");
          const std::string* message = params.FindString("message");
          const std::string* default_prompt = params.FindString("defaultPrompt");
          controller->OnDialogOpened(tab, type ? *type : "alert",
                                     message ? *message : "",
                                     default_prompt ? *default_prompt : "");
        } else if (method == "Page.javascriptDialogClosed") {
          controller->OnDialogClosed(tab);
        }
        // Forward to action-complete wait logic if a waiter is active
        controller->OnCdpEventForWait(tab, method, params);
      },
      weak_factory_.GetWeakPtr(), tab_id));

  // Enable Page domain for navigation and dialog events
  base::Value::Dict empty_params;
  raw_ptr->SendCommand("Page.enable", empty_params,
                       base::BindOnce([](bool, const std::string&) {}));

  // Enable file chooser interception
  base::Value::Dict file_chooser_params;
  file_chooser_params.Set("enabled", true);
  raw_ptr->SendCommand("Page.setInterceptFileChooserDialog",
                       std::move(file_chooser_params),
                       base::BindOnce([](bool, const std::string&) {}));

  state.cdp_client = std::move(client);
  return raw_ptr;
}

void AbpController::SendError(int status,
                              const std::string& error,
                              ResponseCallback callback) {
  base::Value::Dict response;
  response.Set("error", error);
  SendJson(status, base::Value(std::move(response)), std::move(callback));
}

void AbpController::SendJson(int status,
                             base::Value value,
                             ResponseCallback callback) {
  std::string json;
  base::JSONWriter::Write(value, &json);
  std::move(callback).Run(status, "application/json", std::move(json));
}

void AbpController::UpdateVirtualCursorState(const std::string& tab_id,
                                             double x,
                                             double y) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    return;
  }

  scoped_refptr<content::DevToolsAgentHost> host =
      content::DevToolsAgentHost::GetOrCreateFor(wc);
  if (host) {
    TabState& tab_state = GetOrCreateTabState(host->GetId());
    tab_state.cursor.active = true;
    tab_state.cursor.x = x;
    tab_state.cursor.y = y;
  }
}

void AbpController::SetVirtualCursorViaMojo(content::WebContents* wc,
                                             float x,
                                             float y,
                                             bool visible) {
  if (!wc) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorViaMojo - wc is null";
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorViaMojo - rwhv is null";
    return;
  }

  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorViaMojo - rwh is null";
    return;
  }

  // Check if the render process is alive before calling Mojo methods
  if (!rwh->GetProcess() || !rwh->GetProcess()->IsInitializedAndNotDead()) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorViaMojo - render process not ready";
    return;
  }

  LOG(INFO) << "ABP DEBUG L1: SetVirtualCursorViaMojo"
            << " x=" << x << " y=" << y << " visible=" << visible
            << " rwh=valid";
  rwh->SetVirtualCursorPosition(x, y, visible);
}

void AbpController::SetVirtualCursorTypeViaMojo(content::WebContents* wc,
                                                 ui::mojom::CursorType cursor_type) {
  if (!wc) {
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    return;
  }

  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    return;
  }

  rwh->SetVirtualCursorType(cursor_type);
}

void AbpController::SetVirtualCursorEnabledViaMojo(content::WebContents* wc,
                                                    bool enabled) {
  if (!wc) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - wc is null";
    return;
  }

  content::RenderWidgetHostView* rwhv = wc->GetRenderWidgetHostView();
  if (!rwhv) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - rwhv is null";
    return;
  }

  content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost();
  if (!rwh) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - rwh is null";
    return;
  }

  // Check if the render process is alive before calling Mojo methods
  if (!rwh->GetProcess() || !rwh->GetProcess()->IsInitializedAndNotDead()) {
    VLOG(1) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo - render process not ready";
    return;
  }

  LOG(INFO) << "ABP DEBUG L1: SetVirtualCursorEnabledViaMojo"
            << " enabled=" << enabled
            << " rwh=valid";
  rwh->SetVirtualCursorEnabled(enabled);
}

bool AbpController::IsExecutionControlEnabled() const {
  // Execution control is enabled by default, use --abp-disable-pause to disable
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kAbpDisablePause);
}

void AbpController::EnableExecutionControl(
    const std::string& tab_id,
    std::optional<double> initial_virtual_time,
    base::OnceClosure then) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    VLOG(1) << "ABP: Tab not found for execution control: " << tab_id;
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    VLOG(1) << "ABP: Failed to create CDP client for execution control";
    std::move(then).Run();
    return;
  }

  // Check if already enabled
  auto& state = GetOrCreateTabState(tab_id).execution;
  if (state.debugger_enabled && state.virtual_time_enabled) {
    std::move(then).Run();
    return;
  }

  // Step 1: Enable Debugger domain
  base::Value::Dict params;
  client->SendCommand(
      "Debugger.enable", params,
      base::BindOnce(&AbpController::OnDebuggerEnabled,
                     weak_factory_.GetWeakPtr(), tab_id, initial_virtual_time,
                     std::move(then)));
}

void AbpController::OnDebuggerEnabled(
    const std::string& tab_id,
    std::optional<double> initial_virtual_time,
    base::OnceClosure then,
    bool success,
    const std::string& result) {
  if (!success) {
    VLOG(1) << "ABP: Debugger.enable failed: " << result;
    std::move(then).Run();
    return;
  }

  auto& state = GetOrCreateTabState(tab_id).execution;
  state.debugger_enabled = true;

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 2: Enable virtual time with initial pause
  base::Value::Dict params;
  params.Set("policy", "pause");
  if (initial_virtual_time.has_value()) {
    params.Set("initialVirtualTime", *initial_virtual_time);
  }

  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(&AbpController::OnVirtualTimeEnabled,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnVirtualTimeEnabled(
    const std::string& tab_id,
    base::OnceClosure then,
    bool success,
    const std::string& result) {
  if (!success) {
    VLOG(1) << "ABP: setVirtualTimePolicy failed: " << result;
    std::move(then).Run();
    return;
  }

  auto& state = GetOrCreateTabState(tab_id).execution;
  state.virtual_time_enabled = true;
  state.paused = true;  // Started in paused state

  // Parse the result to get virtualTimeTicksBase
  auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    auto ticks_base = parsed->GetDict().FindDouble("virtualTimeTicksBase");
    if (ticks_base) {
      state.virtual_time_base_ticks_ms = *ticks_base;
    }
  }

  LOG(INFO) << "ABP: Execution control enabled for tab " << tab_id
            << ", virtualTimeTicksBase=" << state.virtual_time_base_ticks_ms;
  std::move(then).Run();
}

void AbpController::ResumeExecution(const std::string& tab_id,
                                    base::OnceClosure then) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.debugger_enabled) {
    // Execution control not enabled for this tab yet
    // If global flag is enabled, auto-enable for this tab first
    if (IsExecutionControlEnabled()) {
      EnableExecutionControl(
          tab_id, std::nullopt,
          base::BindOnce(&AbpController::ResumeExecution,
                         weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
      return;
    }
    // Global flag not enabled, just proceed
    std::move(then).Run();
    return;
  }

  ExecutionState& state = it->second.execution;
  if (!state.paused) {
    // Already resumed
    std::move(then).Run();
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 1: Resume debugger
  base::Value::Dict params;
  client->SendCommand(
      "Debugger.resume", params,
      base::BindOnce(&AbpController::OnDebuggerResumed,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnDebuggerResumed(const std::string& tab_id,
                                      base::OnceClosure then,
                                      bool success,
                                      const std::string& result) {
  // Debugger.resume may return error if not actually paused, which is fine
  if (!success) {
    LOG(INFO) << "ABP: Debugger.resume: " << result << " (may not have been paused)";
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 2: Resume virtual time (realtime policy for smooth animations)
  base::Value::Dict params;
  params.Set("policy", "realtime");

  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(&AbpController::OnVirtualTimeResumed,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnVirtualTimeResumed(const std::string& tab_id,
                                         base::OnceClosure then,
                                         bool success,
                                         const std::string& result) {
  if (!success) {
    VLOG(1) << "ABP: setVirtualTimePolicy(realtime) failed: " << result;
  }

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.execution.paused = false;
  }

  VLOG(1) << "ABP: Execution resumed for tab " << tab_id << " - calling then()";
  std::move(then).Run();
}

void AbpController::PauseExecution(const std::string& tab_id,
                                   base::OnceClosure then) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.debugger_enabled) {
    // Execution control not enabled for this tab yet
    // If global flag is enabled, auto-enable for this tab (starts paused)
    if (IsExecutionControlEnabled()) {
      // EnableExecutionControl starts in paused state, so just enable and done
      EnableExecutionControl(tab_id, std::nullopt, std::move(then));
      return;
    }
    // Global flag not enabled, just proceed
    std::move(then).Run();
    return;
  }

  ExecutionState& state = it->second.execution;
  if (state.paused) {
    // Already paused
    std::move(then).Run();
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 1: Pause virtual time first (freeze time before halting JS)
  base::Value::Dict params;
  params.Set("policy", "pause");

  client->SendCommand(
      "Emulation.setVirtualTimePolicy", params,
      base::BindOnce(&AbpController::OnVirtualTimePaused,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnVirtualTimePaused(const std::string& tab_id,
                                        base::OnceClosure then,
                                        bool success,
                                        const std::string& result) {
  if (!success) {
    VLOG(1) << "ABP: setVirtualTimePolicy(pause) failed: " << result;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(then).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(then).Run();
    return;
  }

  // Step 2: Pause debugger (halt JS)
  base::Value::Dict params;
  client->SendCommand(
      "Debugger.pause", params,
      base::BindOnce(&AbpController::OnDebuggerPaused,
                     weak_factory_.GetWeakPtr(), tab_id, std::move(then)));
}

void AbpController::OnDebuggerPaused(const std::string& tab_id,
                                     base::OnceClosure then,
                                     bool success,
                                     const std::string& result) {
  if (!success) {
    VLOG(1) << "ABP: Debugger.pause failed: " << result;
  }

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.execution.paused = true;
  }

  LOG(INFO) << "ABP: Execution paused for tab " << tab_id;
  std::move(then).Run();
}

void AbpController::GetExecutionState(const std::string& tab_id,
                                      ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  base::Value::Dict response;

  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    const ExecutionState& state = it->second.execution;
    response.Set("enabled", state.debugger_enabled && state.virtual_time_enabled);
    response.Set("paused", state.paused);
    response.Set("virtual_time_base_ms", state.virtual_time_base_ticks_ms);
  } else {
    response.Set("enabled", false);
    response.Set("paused", false);
    response.Set("virtual_time_base_ms", 0.0);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::SetExecutionState(const std::string& tab_id,
                                      const base::Value::Dict& params,
                                      ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto paused = params.FindBool("paused");
  if (!paused.has_value()) {
    SendError(400, "Missing 'paused' parameter", std::move(callback));
    return;
  }

  // Check if execution control is enabled for this tab
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.execution.debugger_enabled) {
    // Need to enable first
    std::optional<double> initial_time;
    auto init_time = params.FindDouble("initial_virtual_time");
    if (init_time) {
      initial_time = *init_time;
    }

    bool target_paused = *paused;
    EnableExecutionControl(
        tab_id, initial_time,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               bool target_paused, ResponseCallback cb) {
              if (!controller) {
                return;
              }
              if (target_paused) {
                // Already starts paused, just respond
                controller->GetExecutionState(tid, std::move(cb));
              } else {
                // Need to resume
                controller->ResumeExecution(
                    tid,
                    base::BindOnce(
                        [](base::WeakPtr<AbpController> ctrl, std::string id,
                           ResponseCallback callback) {
                          if (ctrl) {
                            ctrl->GetExecutionState(id, std::move(callback));
                          }
                        },
                        controller, tid, std::move(cb)));
              }
            },
            weak_factory_.GetWeakPtr(), tab_id, target_paused,
            std::move(callback)));
    return;
  }

  // Already enabled, just pause or resume
  if (*paused) {
    PauseExecution(
        tab_id,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               ResponseCallback cb) {
              if (controller) {
                controller->GetExecutionState(tid, std::move(cb));
              }
            },
            weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
  } else {
    ResumeExecution(
        tab_id,
        base::BindOnce(
            [](base::WeakPtr<AbpController> controller, std::string tid,
               ResponseCallback cb) {
              if (controller) {
                controller->GetExecutionState(tid, std::move(cb));
              }
            },
            weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
  }
}

// =============================================================================
// Action complete wait implementation
// =============================================================================

// Constants for action_complete wait
// Note: min_wait_time is now configurable per-action via WaitForActionComplete param
namespace {
constexpr base::TimeDelta kNetworkIdleTime = base::Milliseconds(500);
constexpr base::TimeDelta kNetworkIdleCheckInterval = base::Milliseconds(100);
constexpr base::TimeDelta kWaitTimeout = base::Seconds(30);
constexpr int kNetworkIdleMaxConnections = 2;  // networkidle2
}  // namespace

void AbpController::WaitForActionComplete(const std::string& tab_id,
                                          base::OnceClosure on_complete,
                                          base::TimeDelta min_wait_time) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    // Tab not found, call callback immediately
    std::move(on_complete).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(on_complete).Run();
    return;
  }

  // Create waiter state
  auto waiter = std::make_unique<ActionCompleteWaiter>();
  waiter->tab_id = tab_id;
  waiter->action_start_time = base::TimeTicks::Now();
  waiter->on_complete = std::move(on_complete);
  waiter->last_network_activity = base::TimeTicks::Now();
  waiter->timeout_time = base::TimeTicks::Now() + kWaitTimeout;
  waiter->min_wait_time = min_wait_time;

  // For pages that are already loaded, set load events as fired
  // We'll still wait for network idle and min time
  if (!wc->IsLoading()) {
    waiter->load_fired = true;
    waiter->dom_content_loaded_fired = true;
  }

  // Save min_wait_time BEFORE moving waiter to avoid use-after-move
  base::TimeDelta actual_min_wait = waiter->min_wait_time;

  GetOrCreateTabState(tab_id).action_waiter = std::move(waiter);

  // The existing event listener (set up in GetOrCreateCdpClient) already
  // forwards events to OnCdpEventForWait when a waiter is active.
  // No need to replace the listener here.

  // Enable Network and Page domains for events
  base::Value::Dict empty_params;
  client->SendCommand("Network.enable", empty_params,
                      base::BindOnce([](bool, const std::string&) {}));
  base::Value::Dict empty_params2;
  client->SendCommand("Page.enable", empty_params2,
                      base::BindOnce([](bool, const std::string&) {}));

  // Start minimum wait timer (use configured min_wait_time)
  // Note: actual_min_wait was saved earlier before waiter was moved
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnMinWaitTimeElapsed,
                     weak_factory_.GetWeakPtr(), tab_id),
      actual_min_wait);

  // Start network idle check timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnNetworkIdleCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      kNetworkIdleCheckInterval);

  // Start timeout timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnWaitTimeout,
                     weak_factory_.GetWeakPtr(), tab_id),
      kWaitTimeout);
}

void AbpController::OnCdpEventForWait(const std::string& tab_id,
                                      const std::string& method,
                                      const base::Value::Dict& params) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  // Track network events
  if (method == "Network.requestWillBeSent") {
    waiter->active_requests++;
    waiter->last_network_activity = base::TimeTicks::Now();
    waiter->network_idle = false;
  } else if (method == "Network.loadingFinished" ||
             method == "Network.loadingFailed") {
    if (waiter->active_requests > 0) {
      waiter->active_requests--;
    }
    waiter->last_network_activity = base::TimeTicks::Now();
  }

  // Track page load events
  if (method == "Page.loadEventFired") {
    waiter->load_fired = true;
    // For time wait, start the deferred timer when load fires
    if (waiter->wait_type == "time" && !waiter->time_wait_started &&
        waiter->dom_content_loaded_fired) {
      OnLoadFiredForTimeWait(tab_id);
    }
    CheckActionCompleteConditions(tab_id);
  } else if (method == "Page.domContentEventFired") {
    waiter->dom_content_loaded_fired = true;
    // For time wait, start the deferred timer when both load events fired
    if (waiter->wait_type == "time" && !waiter->time_wait_started &&
        waiter->load_fired) {
      OnLoadFiredForTimeWait(tab_id);
    }
    CheckActionCompleteConditions(tab_id);
  }
}

void AbpController::OnMinWaitTimeElapsed(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  it->second.action_waiter->min_time_elapsed = true;
  CheckActionCompleteConditions(tab_id);
}

void AbpController::OnNetworkIdleCheck(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  // Check if network has been idle (≤2 connections) for kNetworkIdleTime
  if (waiter->active_requests <= kNetworkIdleMaxConnections) {
    base::TimeDelta idle_duration =
        base::TimeTicks::Now() - waiter->last_network_activity;
    if (idle_duration >= kNetworkIdleTime) {
      waiter->network_idle = true;
      CheckActionCompleteConditions(tab_id);
      return;
    }
  }

  // Not idle yet, schedule another check
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnNetworkIdleCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      kNetworkIdleCheckInterval);
}

void AbpController::OnWaitTimeout(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  VLOG(1) << "ABP: action_complete wait timed out for tab " << tab_id;

  // Force complete - removing the waiter stops OnCdpEventForWait from processing events
  std::unique_ptr<ActionCompleteWaiter> waiter = std::move(it->second.action_waiter);

  if (waiter->on_complete) {
    std::move(waiter->on_complete).Run();
  }
}

void AbpController::CheckActionCompleteConditions(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  if (!waiter->IsComplete()) {
    return;
  }

  // All conditions met - removing the waiter stops OnCdpEventForWait from processing events
  std::unique_ptr<ActionCompleteWaiter> completed_waiter = std::move(it->second.action_waiter);

  LOG(INFO) << "ABP: action_complete conditions met for tab " << tab_id;

  if (completed_waiter->on_complete) {
    std::move(completed_waiter->on_complete).Run();
  }
}

void AbpController::WaitFor(const std::string& tab_id,
                            const base::Value::Dict& wait_params,
                            base::OnceClosure on_complete) {
  const std::string* type = wait_params.FindString("type");
  if (!type) {
    // No type specified, fall through to immediate completion
    std::move(on_complete).Run();
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(on_complete).Run();
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    std::move(on_complete).Run();
    return;
  }

  auto waiter = std::make_unique<ActionCompleteWaiter>();
  waiter->tab_id = tab_id;
  waiter->action_start_time = base::TimeTicks::Now();
  waiter->on_complete = std::move(on_complete);
  waiter->timeout_time = base::TimeTicks::Now() + kWaitTimeout;
  waiter->wait_type = *type;

  if (*type == "text") {
    const std::string* text = wait_params.FindString("text");
    if (!text || text->empty()) {
      std::move(waiter->on_complete).Run();
      return;
    }
    waiter->wait_text = *text;
  } else if (*type == "url") {
    const std::string* pattern = wait_params.FindString("url");
    if (!pattern || pattern->empty()) {
      std::move(waiter->on_complete).Run();
      return;
    }
    waiter->wait_url_pattern = *pattern;
  } else if (*type == "time") {
    auto ms = wait_params.FindInt("ms");
    if (!ms || *ms <= 0) {
      std::move(waiter->on_complete).Run();
      return;
    }
    waiter->time_wait_ms = *ms;

    // If page is already loaded, time wait starts immediately
    if (!wc->IsLoading()) {
      waiter->load_fired = true;
      waiter->dom_content_loaded_fired = true;
    }
  } else if (*type == "network_idle") {
    waiter->last_network_activity = base::TimeTicks::Now();
  } else {
    // Unknown type, complete immediately
    std::move(waiter->on_complete).Run();
    return;
  }

  GetOrCreateTabState(tab_id).action_waiter = std::move(waiter);

  // Enable Network and Page domains for events
  base::Value::Dict empty_params;
  client->SendCommand("Network.enable", empty_params,
                      base::BindOnce([](bool, const std::string&) {}));
  base::Value::Dict empty_params2;
  client->SendCommand("Page.enable", empty_params2,
                      base::BindOnce([](bool, const std::string&) {}));

  // Start type-specific polling
  if (*type == "text") {
    OnTextPollCheck(tab_id);
  } else if (*type == "url") {
    OnUrlPollCheck(tab_id);
  } else if (*type == "network_idle") {
    content::GetUIThreadTaskRunner({})->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&AbpController::OnNetworkIdleCheck,
                       weak_factory_.GetWeakPtr(), tab_id),
        kNetworkIdleCheckInterval);
  } else if (*type == "time") {
    // For time wait, check if page already loaded to start the timer
    auto it = tab_states_.find(tab_id);
    if (it != tab_states_.end() && it->second.action_waiter) {
      ActionCompleteWaiter* w = it->second.action_waiter.get();
      if (w->load_fired && w->dom_content_loaded_fired) {
        OnLoadFiredForTimeWait(tab_id);
      }
    }
  }

  // Start timeout timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnWaitTimeout,
                     weak_factory_.GetWeakPtr(), tab_id),
      kWaitTimeout);
}

void AbpController::OnTextPollCheck(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->wait_type != "text" || waiter->text_found) {
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    return;
  }

  // Escape special characters in the search text for JS
  std::string escaped_text = waiter->wait_text;
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\\", "\\\\");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "'", "\\'");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\n", "\\n");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\r", "\\r");

  std::string expression =
      "document.body && document.body.innerText.includes('" + escaped_text + "')";

  base::Value::Dict cdp_params;
  cdp_params.Set("expression", expression);
  cdp_params.Set("returnByValue", true);

  client->SendCommand(
      "Runtime.evaluate", cdp_params,
      base::BindOnce(&AbpController::OnTextPollResult,
                     weak_factory_.GetWeakPtr(), tab_id));
}

void AbpController::OnTextPollResult(const std::string& tab_id,
                                     bool success,
                                     const std::string& result) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();

  if (success) {
    auto parsed = base::JSONReader::Read(result, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_dict()) {
      const base::Value::Dict* cdp_result = parsed->GetDict().FindDict("result");
      if (cdp_result) {
        auto value = cdp_result->FindBool("value");
        if (value && *value) {
          waiter->text_found = true;
          CheckActionCompleteConditions(tab_id);
          return;
        }
      }
    }
  }

  // Not found yet, poll again after 200ms
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnTextPollCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      base::Milliseconds(200));
}

void AbpController::OnUrlPollCheck(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->wait_type != "url" || waiter->url_matched) {
    return;
  }

  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    return;
  }

  std::string current_url = wc->GetVisibleURL().spec();
  if (current_url.find(waiter->wait_url_pattern) != std::string::npos) {
    waiter->url_matched = true;
    CheckActionCompleteConditions(tab_id);
    return;
  }

  // Not matched yet, poll again after 100ms
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&AbpController::OnUrlPollCheck,
                     weak_factory_.GetWeakPtr(), tab_id),
      base::Milliseconds(100));
}

void AbpController::OnLoadFiredForTimeWait(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.action_waiter) {
    return;
  }

  ActionCompleteWaiter* waiter = it->second.action_waiter.get();
  if (waiter->time_wait_started) {
    return;
  }

  waiter->time_wait_started = true;

  // Start the deferred timer
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<AbpController> controller, std::string tab_id) {
            if (!controller) return;
            auto it = controller->tab_states_.find(tab_id);
            if (it == controller->tab_states_.end() || !it->second.action_waiter) {
              return;
            }
            it->second.action_waiter->time_wait_elapsed = true;
            controller->CheckActionCompleteConditions(tab_id);
          },
          weak_factory_.GetWeakPtr(), tab_id),
      base::Milliseconds(waiter->time_wait_ms));
}

int64_t AbpController::GetVirtualTimeMs(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end() && it->second.execution.virtual_time_enabled) {
    return static_cast<int64_t>(it->second.execution.virtual_time_base_ticks_ms);
  }
  return base::Time::Now().InMillisecondsSinceUnixEpoch();
}

void AbpController::OnFileChooserOpened(const std::string& chooser_id,
                                        const std::string& tab_id,
                                        base::Value::Dict info) {
  pending_file_choosers_[chooser_id] = std::move(info);
  LOG(INFO) << "ABP: File chooser opened with ID " << chooser_id
            << " for tab " << tab_id;
}

void AbpController::GetScrollPosition(
    const std::string& tab_id,
    base::OnceCallback<void(base::Value::Dict)> callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    base::Value::Dict empty;
    std::move(callback).Run(std::move(empty));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    base::Value::Dict empty;
    std::move(callback).Run(std::move(empty));
    return;
  }

  // Execute JavaScript to get scroll info
  const std::string script = R"(
    (function() {
      return {
        scrollX: window.scrollX || window.pageXOffset || 0,
        scrollY: window.scrollY || window.pageYOffset || 0,
        pageWidth: Math.max(
          document.body.scrollWidth || 0,
          document.documentElement.scrollWidth || 0
        ),
        pageHeight: Math.max(
          document.body.scrollHeight || 0,
          document.documentElement.scrollHeight || 0
        ),
        viewportWidth: window.innerWidth || 0,
        viewportHeight: window.innerHeight || 0
      };
    })()
  )";

  base::Value::Dict eval_params;
  eval_params.Set("expression", script);
  eval_params.Set("returnByValue", true);

  client->SendCommand(
      "Runtime.evaluate", std::move(eval_params),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value::Dict)> cb, bool success,
             const std::string& result) {
            base::Value::Dict scroll_info;

            if (success) {
              auto parsed = base::JSONReader::Read(
                  result, base::JSON_ALLOW_TRAILING_COMMAS);
              if (parsed && parsed->is_dict()) {
                const base::Value::Dict* result_obj =
                    parsed->GetDict().FindDict("result");
                if (result_obj) {
                  const base::Value::Dict* value =
                      result_obj->FindDict("value");
                  if (value) {
                    double scrollX = value->FindDouble("scrollX").value_or(0);
                    double scrollY = value->FindDouble("scrollY").value_or(0);
                    double pageWidth = value->FindDouble("pageWidth").value_or(0);
                    double pageHeight = value->FindDouble("pageHeight").value_or(0);
                    double viewportWidth = value->FindDouble("viewportWidth").value_or(0);
                    double viewportHeight = value->FindDouble("viewportHeight").value_or(0);

                    scroll_info.Set("horizontal_px", static_cast<int>(scrollX));
                    scroll_info.Set("vertical_px", static_cast<int>(scrollY));
                    scroll_info.Set("page_width", static_cast<int>(pageWidth));
                    scroll_info.Set("page_height", static_cast<int>(pageHeight));
                    scroll_info.Set("viewport_width", static_cast<int>(viewportWidth));
                    scroll_info.Set("viewport_height", static_cast<int>(viewportHeight));

                    // Calculate percentages
                    double h_percent = 0;
                    double v_percent = 0;
                    if (pageWidth > viewportWidth) {
                      h_percent = (scrollX / (pageWidth - viewportWidth)) * 100.0;
                    }
                    if (pageHeight > viewportHeight) {
                      v_percent = (scrollY / (pageHeight - viewportHeight)) * 100.0;
                    }
                    scroll_info.Set("horizontal_percent", h_percent);
                    scroll_info.Set("vertical_percent", v_percent);
                  }
                }
              }
            }

            std::move(cb).Run(std::move(scroll_info));
          },
          std::move(callback)));
}

void AbpController::CaptureScreenshotBase64(
    const std::string& tab_id,
    base::OnceCallback<void(std::string base64, int width, int height)> callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    std::move(callback).Run(std::string(), 0, 0);
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    std::move(callback).Run(std::string(), 0, 0);
    return;
  }

  // Get the view size
  gfx::Size view_size = view->GetViewBounds().size();
  VLOG(1) << "ABP: CaptureScreenshotBase64 - calling CopyFromSurface";

  // Use CopyFromSurface to capture the screen with 5 second timeout
  view->CopyFromSurface(
      gfx::Rect(),       // Empty rect = entire surface
      gfx::Size(),       // Empty size = native size
      base::Seconds(5),  // 5 second timeout
      base::BindOnce(
          [](base::OnceCallback<void(std::string, int, int)> cb, gfx::Size size,
             const content::CopyFromSurfaceResult& result) {
            VLOG(1) << "ABP: CaptureScreenshotBase64 - CopyFromSurface callback";
            // Check if the copy failed
            if (!result.has_value()) {
              VLOG(1) << "ABP: CaptureScreenshotBase64 - CopyFromSurface failed: "
                           << result.error();
              std::move(cb).Run(std::string(), 0, 0);
              return;
            }

            const SkBitmap& bitmap = result.value().bitmap;
            if (bitmap.drawsNothing()) {
              std::move(cb).Run(std::string(), 0, 0);
              return;
            }

            // Encode as WebP
            std::optional<std::vector<uint8_t>> encoded =
                gfx::WebpCodec::Encode(bitmap, 80);

            if (!encoded || encoded->empty()) {
              std::move(cb).Run(std::string(), 0, 0);
              return;
            }

            // Base64 encode
            std::string base64 = base::Base64Encode(*encoded);

            std::move(cb).Run(std::move(base64), bitmap.width(), bitmap.height());
          },
          std::move(callback), view_size));
}

// Dialog endpoints

void AbpController::GetDialog(const std::string& tab_id,
                              ResponseCallback callback) {
  auto it = tab_states_.find(tab_id);

  base::Value::Dict response;
  if (it != tab_states_.end() && it->second.pending_dialog.has_value()) {
    const PendingDialog& dialog = it->second.pending_dialog.value();
    response.Set("present", true);
    response.Set("dialog_type", dialog.dialog_type);
    response.Set("message", dialog.message);
    if (!dialog.default_prompt.empty()) {
      response.Set("default_prompt", dialog.default_prompt);
    }
  } else {
    response.Set("present", false);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::AcceptDialog(const std::string& tab_id,
                                 const base::Value::Dict& params,
                                 ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.pending_dialog.has_value()) {
    SendError(400, "No pending dialog", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to get CDP client", std::move(callback));
    return;
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("accept", true);

  // Include prompt text if provided (for prompt dialogs)
  const std::string* prompt_text = params.FindString("prompt_text");
  if (prompt_text) {
    cdp_params.Set("promptText", *prompt_text);
  }

  client->SendCommand(
      "Page.handleJavaScriptDialog", std::move(cdp_params),
      base::BindOnce(
          [](base::WeakPtr<AbpController> weak_this, std::string tab_id,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!weak_this) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Controller destroyed"})");
              return;
            }
            if (!success) {
              weak_this->SendError(500, "Failed to handle dialog",
                                   std::move(cb));
              return;
            }
            // Remove from pending
            auto state_it = weak_this->tab_states_.find(tab_id);
            if (state_it != weak_this->tab_states_.end()) {
              state_it->second.pending_dialog.reset();
            }

            base::Value::Dict response;
            response.Set("success", true);
            weak_this->SendJson(200, base::Value(std::move(response)),
                               std::move(cb));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
}

void AbpController::DismissDialog(const std::string& tab_id,
                                  ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  auto it = tab_states_.find(tab_id);
  if (it == tab_states_.end() || !it->second.pending_dialog.has_value()) {
    SendError(400, "No pending dialog", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to get CDP client", std::move(callback));
    return;
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("accept", false);

  client->SendCommand(
      "Page.handleJavaScriptDialog", std::move(cdp_params),
      base::BindOnce(
          [](base::WeakPtr<AbpController> weak_this, std::string tab_id,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!weak_this) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Controller destroyed"})");
              return;
            }
            if (!success) {
              weak_this->SendError(500, "Failed to dismiss dialog",
                                   std::move(cb));
              return;
            }
            // Remove from pending
            auto state_it = weak_this->tab_states_.find(tab_id);
            if (state_it != weak_this->tab_states_.end()) {
              state_it->second.pending_dialog.reset();
            }

            base::Value::Dict response;
            response.Set("success", true);
            weak_this->SendJson(200, base::Value(std::move(response)),
                               std::move(cb));
          },
          weak_factory_.GetWeakPtr(), tab_id, std::move(callback)));
}

void AbpController::OnDialogOpened(const std::string& tab_id,
                                   const std::string& dialog_type,
                                   const std::string& message,
                                   const std::string& default_prompt) {
  PendingDialog dialog;
  dialog.dialog_type = dialog_type;
  dialog.message = message;
  dialog.default_prompt = default_prompt;
  dialog.opened_at_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  GetOrCreateTabState(tab_id).pending_dialog = std::move(dialog);
  LOG(INFO) << "ABP: Dialog opened in tab " << tab_id << " type=" << dialog_type;
}

void AbpController::OnDialogClosed(const std::string& tab_id) {
  auto it = tab_states_.find(tab_id);
  if (it != tab_states_.end()) {
    it->second.pending_dialog.reset();
  }
  LOG(INFO) << "ABP: Dialog closed in tab " << tab_id;
}

// File chooser endpoint

void AbpController::HandleFileChooser(const std::string& chooser_id,
                                      const base::Value::Dict& params,
                                      ResponseCallback callback) {
  auto it = pending_file_choosers_.find(chooser_id);
  if (it == pending_file_choosers_.end()) {
    SendError(404, "File chooser not found", std::move(callback));
    return;
  }

  const std::string* tab_id = it->second.FindString("tab_id");
  if (!tab_id) {
    SendError(500, "Invalid file chooser state", std::move(callback));
    return;
  }

  content::WebContents* wc = FindWebContents(*tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  AbpCdpClient* client = GetOrCreateCdpClient(wc);
  if (!client) {
    SendError(500, "Failed to get CDP client", std::move(callback));
    return;
  }

  // Check if cancel is requested
  auto cancel = params.FindBool("cancel");
  if (cancel && *cancel) {
    base::Value::Dict cdp_params;
    cdp_params.Set("action", "cancel");

    std::string tab_id_copy = *tab_id;
    client->SendCommand(
        "Page.handleFileChooser", std::move(cdp_params),
        base::BindOnce(
            [](base::WeakPtr<AbpController> weak_this, std::string chooser_id,
               ResponseCallback cb, bool success, const std::string& result) {
              if (!weak_this) {
                std::move(cb).Run(500, "application/json",
                                 R"({"error":"Controller destroyed"})");
                return;
              }
              // Remove from pending
              weak_this->pending_file_choosers_.erase(chooser_id);

              if (!success) {
                weak_this->SendError(500, "Failed to cancel file chooser",
                                     std::move(cb));
                return;
              }

              base::Value::Dict response;
              response.Set("success", true);
              response.Set("cancelled", true);
              weak_this->SendJson(200, base::Value(std::move(response)),
                                 std::move(cb));
            },
            weak_factory_.GetWeakPtr(), chooser_id, std::move(callback)));
    return;
  }

  // Get files to provide
  const base::Value::List* files = params.FindList("files");
  const std::string* save_path = params.FindString("path");

  if (!files && !save_path) {
    SendError(400, "Must provide 'files' array or 'path' for save dialog",
              std::move(callback));
    return;
  }

  base::Value::Dict cdp_params;
  cdp_params.Set("action", "accept");

  base::Value::List file_list;
  if (files) {
    for (const auto& file : *files) {
      if (file.is_string()) {
        file_list.Append(file.GetString());
      }
    }
  } else if (save_path) {
    file_list.Append(*save_path);
  }
  cdp_params.Set("files", std::move(file_list));

  client->SendCommand(
      "Page.handleFileChooser", std::move(cdp_params),
      base::BindOnce(
          [](base::WeakPtr<AbpController> weak_this, std::string chooser_id,
             ResponseCallback cb, bool success, const std::string& result) {
            if (!weak_this) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Controller destroyed"})");
              return;
            }
            // Remove from pending
            weak_this->pending_file_choosers_.erase(chooser_id);

            if (!success) {
              weak_this->SendError(500, "Failed to handle file chooser",
                                   std::move(cb));
              return;
            }

            base::Value::Dict response;
            response.Set("success", true);
            weak_this->SendJson(200, base::Value(std::move(response)),
                               std::move(cb));
          },
          weak_factory_.GetWeakPtr(), chooser_id, std::move(callback)));
}

// Binary screenshot endpoint (GET)

void AbpController::BinaryScreenshot(const std::string& tab_id,
                                     const std::string& query,
                                     ResponseCallback callback) {
  content::WebContents* wc = FindWebContents(tab_id);
  if (!wc) {
    SendError(404, "Tab not found", std::move(callback));
    return;
  }

  content::RenderWidgetHostView* view = wc->GetRenderWidgetHostView();
  if (!view) {
    SendError(500, "No render view available", std::move(callback));
    return;
  }

  // Parse query params for markup option
  // Format: ?markup=interactive or ?markup=none
  std::string markup = "none";
  if (!query.empty()) {
    size_t pos = query.find("markup=");
    if (pos != std::string::npos) {
      size_t start = pos + 7;
      size_t end = query.find('&', start);
      markup = query.substr(start, end == std::string::npos ? end : end - start);
    }
  }

  // For binary output, we use CopyFromSurface directly
  // TODO: Add markup support later if needed
  view->CopyFromSurface(
      gfx::Rect(),       // Empty rect = entire surface
      gfx::Size(),       // Empty size = native size
      base::Seconds(5),  // 5 second timeout to match other screenshot paths
      base::BindOnce(
          [](ResponseCallback cb,
             const content::CopyFromSurfaceResult& result) {
            // Check if the copy failed
            if (!result.has_value()) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Screenshot capture failed"})");
              return;
            }

            const SkBitmap& bitmap = result.value().bitmap;
            if (bitmap.drawsNothing()) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"Empty screenshot"})");
              return;
            }

            // Encode as WebP
            std::optional<std::vector<uint8_t>> encoded =
                gfx::WebpCodec::Encode(bitmap, 80);

            if (!encoded || encoded->empty()) {
              std::move(cb).Run(500, "application/json",
                               R"({"error":"WebP encoding failed"})");
              return;
            }

            // Return raw binary WebP data
            std::string binary_data(encoded->begin(), encoded->end());
            std::move(cb).Run(200, "image/webp", std::move(binary_data));
          },
          std::move(callback)));
}

// Browser shutdown endpoint

void AbpController::ShutdownBrowser(const base::Value::Dict& params,
                                    ResponseCallback callback) {
  // Get optional timeout
  int timeout_ms = params.FindInt("timeout_ms").value_or(5000);

  LOG(INFO) << "ABP: Browser shutdown requested with timeout " << timeout_ms << "ms";

  // Send response before shutting down
  base::Value::Dict response;
  response.Set("success", true);
  response.Set("message", "Browser shutdown initiated");
  SendJson(200, base::Value(std::move(response)), std::move(callback));

  // Schedule shutdown after a brief delay to allow response to be sent
  content::GetUIThreadTaskRunner({})->PostDelayedTask(
      FROM_HERE,
      base::BindOnce([]() {
        chrome::AttemptExit();
      }),
      base::Milliseconds(100));
}

// Download endpoints

void AbpController::SetDownloadObserver(AbpDownloadObserver* observer) {
  download_observer_ = observer;
}

void AbpController::ListDownloads(const std::string& query,
                                  ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  // Parse query params: state=in_progress, limit=100
  std::string state_filter;
  int limit = 100;

  if (!query.empty()) {
    size_t state_pos = query.find("state=");
    if (state_pos != std::string::npos) {
      size_t start = state_pos + 6;
      size_t end = query.find('&', start);
      state_filter = query.substr(start, end == std::string::npos ? end : end - start);
    }

    size_t limit_pos = query.find("limit=");
    if (limit_pos != std::string::npos) {
      size_t start = limit_pos + 6;
      size_t end = query.find('&', start);
      std::string limit_str = query.substr(start, end == std::string::npos ? end : end - start);
      base::StringToInt(limit_str, &limit);
    }
  }

  auto downloads = download_observer_->GetDownloads(state_filter, limit);

  base::Value::List downloads_list;
  for (const auto& dl : downloads) {
    base::Value::Dict item;
    item.Set("id", dl.id);
    item.Set("url", dl.url);
    item.Set("filename", dl.filename);
    item.Set("path", dl.path);
    item.Set("state", dl.state);
    item.Set("bytes_received", static_cast<double>(dl.bytes_received));
    item.Set("total_bytes", static_cast<double>(dl.total_bytes));
    item.Set("mime_type", dl.mime_type);
    item.Set("start_time", static_cast<double>(dl.start_time_ms));
    if (dl.end_time_ms > 0) {
      item.Set("end_time", static_cast<double>(dl.end_time_ms));
    }
    downloads_list.Append(std::move(item));
  }

  base::Value::Dict response;
  response.Set("downloads", std::move(downloads_list));
  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::GetDownloadStatus(const std::string& download_id,
                                      ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  auto download = download_observer_->GetDownload(download_id);
  if (!download) {
    SendError(404, "Download not found", std::move(callback));
    return;
  }

  base::Value::Dict response;
  response.Set("id", download->id);
  response.Set("url", download->url);
  response.Set("filename", download->filename);
  response.Set("path", download->path);
  response.Set("state", download->state);
  response.Set("bytes_received", static_cast<double>(download->bytes_received));
  response.Set("total_bytes", static_cast<double>(download->total_bytes));
  response.Set("mime_type", download->mime_type);
  response.Set("start_time", static_cast<double>(download->start_time_ms));
  if (download->end_time_ms > 0) {
    response.Set("end_time", static_cast<double>(download->end_time_ms));
  }

  // Calculate percent complete
  if (download->total_bytes > 0) {
    double percent = (static_cast<double>(download->bytes_received) /
                      static_cast<double>(download->total_bytes)) * 100.0;
    response.Set("percent_complete", percent);
  }

  SendJson(200, base::Value(std::move(response)), std::move(callback));
}

void AbpController::CancelDownload(const std::string& download_id,
                                   ResponseCallback callback) {
  if (!download_observer_) {
    SendError(503, "Download observer not available", std::move(callback));
    return;
  }

  if (download_observer_->CancelDownload(download_id)) {
    base::Value::Dict response;
    response.Set("success", true);
    response.Set("message", "Download cancelled");
    SendJson(200, base::Value(std::move(response)), std::move(callback));
  } else {
    SendError(400, "Failed to cancel download (may be already completed)",
              std::move(callback));
  }
}

}  // namespace abp
