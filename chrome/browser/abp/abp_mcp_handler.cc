#include "chrome/browser/abp/abp_mcp_handler.h"

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/abp/abp_tool_builder.h"

namespace abp {

namespace {

// MCP protocol version
constexpr char kProtocolVersion[] = "2025-03-26";

// JSON-RPC error codes
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
// constexpr int kInternalError = -32603;  // Reserved for future use

// Get tool definitions for tools/list using ToolBuilder
base::Value::List GetToolDefinitions() {
  base::Value::List tools;

  // Browser status and tab management
  tools.Append(ToolBuilder("browser_get_status")
                   .Description("Get browser status and readiness")
                   .Build());

  tools.Append(ToolBuilder("browser_list_tabs")
                   .Description("List all open browser tabs")
                   .Build());

  tools.Append(ToolBuilder("browser_new_tab")
                   .Description("Create a new browser tab")
                   .OptionalString("url", "URL to navigate to")
                   .Build());

  tools.Append(ToolBuilder("browser_close_tab")
                   .Description("Close a browser tab")
                   .OptionalString("tab_id", "ID of tab to close")
                   .Build());

  tools.Append(ToolBuilder("browser_get_tab_info")
                   .Description("Get detailed information about a tab")
                   .OptionalString("tab_id", "ID of tab")
                   .Build());

  // Navigation
  tools.Append(ToolBuilder("browser_navigate")
                   .Description("Navigate to a URL")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("url", "URL to navigate to")
                   .Build());

  tools.Append(ToolBuilder("browser_go_back")
                   .Description("Navigate back in history")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());

  tools.Append(ToolBuilder("browser_go_forward")
                   .Description("Navigate forward in history")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());

  tools.Append(ToolBuilder("browser_reload")
                   .Description("Reload the current page")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());

  // Input actions
  tools.Append(ToolBuilder("browser_click")
                   .Description("Click at coordinates on the page")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate")
                   .RequiredNumber("y", "Y coordinate")
                   .OptionalStringEnum("button", "Mouse button",
                                       {"left", "right", "middle"})
                   .OptionalNumber("click_count", "1=single, 2=double, 3=triple click")
                   .OptionalStringArrayEnum("modifiers", "Modifier keys to hold",
                                            {"Shift", "Control", "Alt", "Meta"})
                   .Build());

  tools.Append(ToolBuilder("browser_type")
                   .Description("Type text at current focus position")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("text", "Text to type")
                   .Build());

  tools.Append(
      ToolBuilder("browser_screenshot")
          .Description(
              "Take a screenshot. Also acts as a wait: resumes page "
              "execution, waits for rendering to settle, captures the "
              "viewport, then re-pauses execution. Use this when you need "
              "to let the page load or update before your next action.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalString("markup",
                          "Element markup overlay: none, interactive, "
                          "clickable, typeable, inputs")
          .OptionalString("format", "Image format: png, webp, jpeg")
          .Build());

  tools.Append(
      ToolBuilder("browser_execute_javascript")
          .Description("Execute JavaScript in the page context")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("expression", "JavaScript expression to evaluate")
          .Build());

  tools.Append(
      ToolBuilder("browser_keyboard_press")
          .Description(
              "Press a key or key combination (e.g., Enter, Escape, Ctrl+C)")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("key", "Key to press (e.g., Enter, Escape, a, F1, Tab)")
          .OptionalStringArrayEnum("modifiers", "Modifier keys to hold",
                                   {"Shift", "Control", "Alt", "Meta"})
          .Build());

  tools.Append(ToolBuilder("browser_scroll")
                   .Description("Scroll using mouse wheel at element coordinates. Simulates moving mouse over element and scrolling. At least one of delta_x or delta_y must be non-zero.")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate of element center (where mouse wheel event fires)")
                   .RequiredNumber("y", "Y coordinate of element center (where mouse wheel event fires)")
                   .OptionalNumber("delta_x", "Horizontal scroll in pixels (positive=right, negative=left, default=0)")
                   .OptionalNumber("delta_y", "Vertical scroll in pixels (negative=up, positive=down, default=0)")
                   .Build());

  tools.Append(ToolBuilder("browser_mouse_move")
                   .Description("Move mouse to coordinates (for hover effects)")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("x", "X coordinate")
                   .RequiredNumber("y", "Y coordinate")
                   .Build());

  // Tab control
  tools.Append(ToolBuilder("browser_activate_tab")
                   .Description("Switch to a specific tab")
                   .OptionalString("tab_id", "ID of tab to activate")
                   .Build());

  tools.Append(ToolBuilder("browser_stop_loading")
                   .Description("Stop page loading")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());

  // Dialog handling
  tools.Append(
      ToolBuilder("browser_get_dialog")
          .Description("Check if a dialog (alert/confirm/prompt) is pending")
          .OptionalString("tab_id", "Target tab ID")
          .Build());

  tools.Append(ToolBuilder("browser_accept_dialog")
                   .Description("Accept (click OK on) a pending dialog")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalString("prompt_text", "Text to enter for prompt dialogs")
                   .Build());

  tools.Append(ToolBuilder("browser_dismiss_dialog")
                   .Description("Dismiss (click Cancel on) a pending dialog")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());

  // Downloads
  tools.Append(
      ToolBuilder("browser_list_downloads")
          .Description("List all downloads")
          .OptionalStringEnum("state", "Filter by download state",
                              {"in_progress", "completed", "cancelled", "failed"})
          .OptionalNumber("limit", "Maximum number of downloads to return")
          .Build());

  tools.Append(ToolBuilder("browser_get_download")
                   .Description("Get download status")
                   .RequiredString("download_id", "Download ID")
                   .Build());

  tools.Append(ToolBuilder("browser_cancel_download")
                   .Description("Cancel an in-progress download")
                   .RequiredString("download_id", "Download ID")
                   .Build());

  // File chooser
  tools.Append(ToolBuilder("browser_provide_files")
                   .Description("Provide files to a pending file chooser dialog")
                   .RequiredString("chooser_id", "File chooser ID from event")
                   .OptionalStringArray("files", "File paths to provide")
                   .OptionalString("path", "Save path for save dialogs")
                   .OptionalBoolean("cancel", "Cancel the file chooser")
                   .Build());

  // Keyboard hold/release
  tools.Append(ToolBuilder("browser_keyboard_down")
                   .Description("Press and hold a key")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("key", "Key to press down")
                   .Build());

  tools.Append(ToolBuilder("browser_keyboard_up")
                   .Description("Release a held key")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredString("key", "Key to release")
                   .Build());

  // Execution control
  tools.Append(ToolBuilder("browser_get_execution_state")
                   .Description("Get JavaScript execution state for a tab")
                   .OptionalString("tab_id", "Target tab ID")
                   .Build());

  tools.Append(ToolBuilder("browser_set_execution_state")
                   .Description("Pause or resume JavaScript execution")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredBoolean("paused", "True to pause, false to resume")
                   .Build());

  // Text extraction
  tools.Append(ToolBuilder("browser_get_text")
                   .Description("Get the visible text content of the page")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalString("selector", "CSS selector to scope text extraction")
                   .Build());

  // Browser control
  tools.Append(ToolBuilder("browser_shutdown")
                   .Description("Gracefully shut down the browser")
                   .OptionalNumber("timeout_ms", "Timeout before force quit in ms")
                   .Build());

  return tools;
}

// Guide content served via resources/read for abp://guide
constexpr char kGuideContent[] = R"md(# ABP Browser Control Guide

## How ABP Works

ABP **pauses JavaScript and virtual time** between your actions. The page is frozen until your next tool call.

When you call any action tool (click, type, navigate, scroll, etc.):
1. ABP **resumes** JS execution
2. ABP dispatches your action
3. ABP **waits ~500ms** for the page to settle (rendering, network, scripts)
4. ABP captures **before and after screenshots** automatically
5. ABP **re-pauses** JS execution
6. You receive the response with both screenshots

**One tool call = one complete turn.** Screenshots are included automatically with every action response. There is no need to take a separate screenshot after performing an action.

## Waiting for Slow Content

Sometimes 500ms isn't enough for the page to finish loading (AJAX, animations, redirects). When the after screenshot shows incomplete content:

**Call `browser_screenshot` to wait and observe.** It runs the same resume-wait-capture-pause cycle without performing any action, giving the page another chance to settle. Repeat until the content appears.

## Markup Overlays

Pass `markup: "interactive"` to `browser_screenshot` to see numbered labels overlaid on all interactive elements. Each label shows the element's coordinates for targeting clicks and typing.

## Tool Reference

All `tab_id` parameters are optional and default to the active tab.

**Tab Management:** `browser_get_status`, `browser_list_tabs`, `browser_new_tab` (url), `browser_close_tab`, `browser_get_tab_info`, `browser_activate_tab`, `browser_stop_loading`

**Navigation:** `browser_navigate` (url required), `browser_go_back`, `browser_go_forward`, `browser_reload`

**Input:** `browser_click` (x, y required), `browser_type` (text required), `browser_keyboard_press` (key required, modifiers), `browser_keyboard_down` (key), `browser_keyboard_up` (key), `browser_scroll` (x, y required; delta_x, delta_y), `browser_mouse_move` (x, y required)

**Content:** `browser_screenshot` (markup, format), `browser_execute_javascript` (expression required), `browser_get_text` (selector)

**Dialogs:** `browser_get_dialog`, `browser_accept_dialog` (prompt_text), `browser_dismiss_dialog`

**Downloads:** `browser_list_downloads` (state, limit), `browser_get_download` (download_id), `browser_cancel_download` (download_id)

**File Chooser:** `browser_provide_files` (chooser_id required, files, path, cancel)

**Execution Control:** `browser_get_execution_state`, `browser_set_execution_state` (paused required)

**Browser:** `browser_shutdown` (timeout_ms)

## Debugging

Session data is stored in the session directory (set via `--abp-session-dir` or defaults to `/tmp/abp-<UUID>/`):

```
sessions/<timestamp>/
├── history.db           # SQLite database with sessions, actions, events
└── screenshots/         # Auto-saved before/after WebP screenshots per action
```

Query the database:
```sql
-- Recent actions
SELECT id, type, status, url, error FROM actions ORDER BY id DESC LIMIT 10;
-- Events for an action
SELECT * FROM events WHERE action_id = <id>;
-- Screenshot paths
SELECT screenshot_before_path, screenshot_after_path FROM actions WHERE id = <id>;
```

## Tips

- `browser_execute_javascript` uses `expression` as its parameter name (not `script`)
- `browser_scroll` requires `x`, `y` coordinates where the mouse wheel fires — target the element center
- Scroll direction: `delta_y` positive = scroll down, negative = scroll up
- JS is paused between actions — timers and animations don't advance until your next tool call
)md";

}  // namespace

AbpMcpHandler::AbpMcpHandler(AbpController* controller)
    : controller_(controller) {}

AbpMcpHandler::~AbpMcpHandler() = default;

std::string AbpMcpHandler::ResolveTabId(const base::Value::Dict& args) {
  const std::string* tab_id = args.FindString("tab_id");
  if (tab_id && !tab_id->empty()) {
    return *tab_id;
  }
  return controller_->GetActiveTabId();
}

void AbpMcpHandler::HandleRequest(
    const std::string& method,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    ResponseWithHeadersCallback callback) {
  // Handle GET request (SSE stream) - not implemented yet
  if (method == "GET") {
    // For now, return method not allowed
    // TODO: Implement SSE streaming for server notifications
    std::move(callback).Run(405, "application/json", {},
                            R"({"error":"SSE streaming not implemented"})");
    return;
  }

  // Handle DELETE request - no-op since sessions are not supported
  if (method == "DELETE") {
    std::move(callback).Run(204, "application/json", {}, "");
    return;
  }

  // Handle POST request (JSON-RPC)
  if (method != "POST") {
    std::move(callback).Run(405, "application/json", {},
                            R"({"error":"Method not allowed"})");
    return;
  }

  // Parse JSON-RPC request
  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    SendJsonRpcError(base::Value(), kParseError, "Parse error",
                     std::move(callback));
    return;
  }

  const base::Value::Dict& request = parsed->GetDict();

  // Validate JSON-RPC version
  const std::string* jsonrpc = request.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0") {
    SendJsonRpcError(base::Value(), kInvalidRequest,
                     "Invalid JSON-RPC version", std::move(callback));
    return;
  }

  // Get request ID - preserve original type (int, string, or null per JSON-RPC 2.0)
  base::Value request_id;
  if (const auto* id_value = request.Find("id")) {
    request_id = id_value->Clone();
  }

  // Get method
  const std::string* rpc_method = request.FindString("method");
  if (!rpc_method) {
    SendJsonRpcError(std::move(request_id), kInvalidRequest, "Missing method",
                     std::move(callback));
    return;
  }

  // Get params (optional)
  const base::Value::Dict* params = request.FindDict("params");
  base::Value::Dict empty_params;
  if (!params) {
    params = &empty_params;
  }

  // Route to appropriate handler
  if (*rpc_method == "initialize") {
    HandleInitialize(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "notifications/initialized") {
    // Client notification that initialization is complete
    SendAccepted(std::move(callback));
  } else if (*rpc_method == "tools/list") {
    HandleToolsList(std::move(request_id), std::move(callback));
  } else if (*rpc_method == "tools/call") {
    HandleToolsCall(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "resources/list") {
    HandleResourcesList(std::move(request_id), std::move(callback));
  } else if (*rpc_method == "resources/read") {
    HandleResourcesRead(*params, std::move(request_id), std::move(callback));
  } else if (*rpc_method == "ping") {
    // Simple ping/pong
    base::Value::Dict result;
    SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                      std::move(callback));
  } else {
    SendJsonRpcError(std::move(request_id), kMethodNotFound, "Method not found",
                     std::move(callback));
  }
}

void AbpMcpHandler::HandleInitialize(const base::Value::Dict& params,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  // Extract client info for logging
  std::string client_name = "unknown";
  std::string client_version = "unknown";
  if (const base::Value::Dict* client_info = params.FindDict("clientInfo")) {
    if (const std::string* name = client_info->FindString("name")) {
      client_name = *name;
    }
    if (const std::string* version = client_info->FindString("version")) {
      client_version = *version;
    }
  }

  LOG(INFO) << "ABP MCP: Initialize from client " << client_name << " v"
            << client_version;

  // Build response
  base::Value::Dict result;
  result.Set("protocolVersion", kProtocolVersion);

  base::Value::Dict server_info;
  server_info.Set("name", "abp-browser");
  server_info.Set("version", "1.0.0");
  result.Set("serverInfo", std::move(server_info));

  base::Value::Dict capabilities;
  base::Value::Dict tools_cap;
  capabilities.Set("tools", std::move(tools_cap));
  base::Value::Dict resources_cap;
  capabilities.Set("resources", std::move(resources_cap));
  result.Set("capabilities", std::move(capabilities));

  // Build JSON-RPC response
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  response.Set("result", std::move(result));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", {}, response_json);
}

void AbpMcpHandler::HandleToolsList(base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  base::Value::Dict result;
  result.Set("tools", GetToolDefinitions());

  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleToolsCall(const base::Value::Dict& params,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  const std::string* name = params.FindString("name");
  if (!name) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing tool name",
                     std::move(callback));
    return;
  }

  const base::Value::Dict* args = params.FindDict("arguments");
  base::Value::Dict empty_args;
  if (!args) {
    args = &empty_args;
  }

  // Route to tool implementation
  if (*name == "browser_get_status") {
    CallBrowserGetStatus(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_list_tabs") {
    CallBrowserListTabs(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_new_tab") {
    CallBrowserNewTab(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_close_tab") {
    CallBrowserCloseTab(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_tab_info") {
    CallBrowserGetTabInfo(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_navigate") {
    CallBrowserNavigate(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_go_back") {
    CallBrowserGoBack(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_go_forward") {
    CallBrowserGoForward(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_reload") {
    CallBrowserReload(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_click") {
    CallBrowserClick(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_type") {
    CallBrowserType(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_screenshot") {
    CallBrowserScreenshot(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_execute_javascript") {
    CallBrowserExecuteJavascript(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_keyboard_press") {
    CallBrowserKeyboardPress(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_scroll") {
    CallBrowserScroll(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_mouse_move") {
    CallBrowserMouseMove(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_activate_tab") {
    CallBrowserActivateTab(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_stop_loading") {
    CallBrowserStopLoading(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_dialog") {
    CallBrowserGetDialog(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_accept_dialog") {
    CallBrowserAcceptDialog(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_dismiss_dialog") {
    CallBrowserDismissDialog(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_list_downloads") {
    CallBrowserListDownloads(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_download") {
    CallBrowserGetDownload(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_cancel_download") {
    CallBrowserCancelDownload(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_provide_files") {
    CallBrowserProvideFiles(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_keyboard_down") {
    CallBrowserKeyboardDown(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_keyboard_up") {
    CallBrowserKeyboardUp(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_execution_state") {
    CallBrowserGetExecutionState(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_set_execution_state") {
    CallBrowserSetExecutionState(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_text") {
    CallBrowserGetText(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_shutdown") {
    CallBrowserShutdown(*args, std::move(request_id), std::move(callback));
  } else {
    SendJsonRpcError(std::move(request_id), kMethodNotFound,
                     "Unknown tool: " + *name, std::move(callback));
  }
}

void AbpMcpHandler::HandleResourcesList(base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  base::Value::Dict result;
  base::Value::List resources;

  base::Value::Dict guide;
  guide.Set("uri", "abp://guide");
  guide.Set("name", "ABP Usage Guide");
  guide.Set("description",
            "How to use ABP browser tools effectively - covers the "
            "pause/resume execution model, screenshot behavior, tool "
            "reference, and debugging");
  guide.Set("mimeType", "text/markdown");
  resources.Append(std::move(guide));

  result.Set("resources", std::move(resources));
  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::HandleResourcesRead(const base::Value::Dict& params,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  const std::string* uri = params.FindString("uri");
  if (!uri) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing uri",
                     std::move(callback));
    return;
  }

  if (*uri != "abp://guide") {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Unknown resource: " + *uri, std::move(callback));
    return;
  }

  base::Value::Dict result;
  base::Value::List contents;

  base::Value::Dict content;
  content.Set("uri", "abp://guide");
  content.Set("mimeType", "text/markdown");
  content.Set("text", kGuideContent);
  contents.Append(std::move(content));

  result.Set("contents", std::move(contents));
  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::CallBrowserGetStatus(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  controller_->GetBrowserStatus(
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserListTabs(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  controller_->HandleRequest(
      "GET", "/api/v1/tabs", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserNewTab(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string body;
  base::JSONWriter::Write(base::Value(args.Clone()), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserCloseTab(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "DELETE", "/api/v1/tabs/" + tab_id, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetTabInfo(const base::Value::Dict& args,
                                          base::Value request_id,
                                          ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/tabs/" + tab_id, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserNavigate(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/navigate", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGoBack(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/back", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGoForward(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/forward", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserReload(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/reload", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserClick(const base::Value::Dict& args,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/click", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserType(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/type", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserScreenshot(const base::Value::Dict& args,
                                          base::Value request_id,
                                          ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Build screenshot options from flat args
  base::Value::Dict body_dict;
  base::Value::Dict screenshot_opts;
  if (const std::string* markup = args.FindString("markup")) {
    screenshot_opts.Set("markup", *markup);
  }
  if (const std::string* format = args.FindString("format")) {
    screenshot_opts.Set("format", *format);
  }
  body_dict.Set("screenshot", std::move(screenshot_opts));

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/screenshot", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserExecuteJavascript(const base::Value::Dict& args,
                                                 base::Value request_id,
                                                 ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Map MCP "expression" to REST "script"
  base::Value::Dict body_dict;
  if (const std::string* expression = args.FindString("expression")) {
    body_dict.Set("script", *expression);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/execute", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserKeyboardPress(const base::Value::Dict& args,
                                             base::Value request_id,
                                             ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/keyboard/press", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserScroll(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/scroll", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserMouseMove(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/move", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserActivateTab(const base::Value::Dict& args,
                                           base::Value request_id,
                                           ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/activate", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserStopLoading(const base::Value::Dict& args,
                                           base::Value request_id,
                                           ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/stop", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetDialog(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/tabs/" + tab_id + "/dialog", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserAcceptDialog(const base::Value::Dict& args,
                                            base::Value request_id,
                                            ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/dialog/accept", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserDismissDialog(const base::Value::Dict& args,
                                             base::Value request_id,
                                             ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/dialog/dismiss", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserListDownloads(const base::Value::Dict& args,
                                             base::Value request_id,
                                             ResponseWithHeadersCallback callback) {
  // Build query string from optional params
  std::string path = "/api/v1/downloads";
  std::vector<std::string> query_parts;

  if (const std::string* state = args.FindString("state")) {
    query_parts.push_back("state=" + *state);
  }
  if (auto limit = args.FindDouble("limit")) {
    query_parts.push_back("limit=" + base::NumberToString(static_cast<int>(*limit)));
  }

  if (!query_parts.empty()) {
    path += "?";
    for (size_t i = 0; i < query_parts.size(); ++i) {
      if (i > 0) path += "&";
      path += query_parts[i];
    }
  }

  controller_->HandleRequest(
      "GET", path, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetDownload(const base::Value::Dict& args,
                                           base::Value request_id,
                                           ResponseWithHeadersCallback callback) {
  const std::string* download_id = args.FindString("download_id");
  if (!download_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing download_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/downloads/" + *download_id, "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserCancelDownload(const base::Value::Dict& args,
                                              base::Value request_id,
                                              ResponseWithHeadersCallback callback) {
  const std::string* download_id = args.FindString("download_id");
  if (!download_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing download_id",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "POST", "/api/v1/downloads/" + *download_id + "/cancel", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserProvideFiles(const base::Value::Dict& args,
                                            base::Value request_id,
                                            ResponseWithHeadersCallback callback) {
  const std::string* chooser_id = args.FindString("chooser_id");
  if (!chooser_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams, "Missing chooser_id",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("chooser_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/file-chooser/" + *chooser_id, body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserKeyboardDown(const base::Value::Dict& args,
                                            base::Value request_id,
                                            ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/keyboard/down", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserKeyboardUp(const base::Value::Dict& args,
                                          base::Value request_id,
                                          ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/keyboard/up", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetExecutionState(const base::Value::Dict& args,
                                                 base::Value request_id,
                                                 ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  controller_->HandleRequest(
      "GET", "/api/v1/tabs/" + tab_id + "/execution", "",
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserSetExecutionState(const base::Value::Dict& args,
                                                 base::Value request_id,
                                                 ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward all args except URL path params to REST API
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/execution", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserGetText(const base::Value::Dict& args,
                                       base::Value request_id,
                                       ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward selector param to REST API
  base::Value::Dict body_dict;
  if (const std::string* selector = args.FindString("selector")) {
    body_dict.Set("selector", *selector);
  }

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/text", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::CallBrowserShutdown(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
  // Forward all args to REST API
  std::string body;
  base::JSONWriter::Write(base::Value(args.Clone()), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/browser/shutdown", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

void AbpMcpHandler::OnControllerResponse(base::Value request_id,
                                         ResponseWithHeadersCallback callback,
                                         int status,
                                         const std::string& content_type,
                                         std::string body) {
  // Convert REST API response to MCP tool result.
  // Extract screenshot data into native MCP image content blocks so the
  // model can see pages directly, rather than embedding base64 in JSON text.

  base::Value::Dict result;
  base::Value::List content;

  auto parsed = base::JSONReader::Read(body, base::JSON_PARSE_RFC);
  if (parsed && parsed->is_dict()) {
    base::Value::Dict& response_dict = parsed->GetDict();

    // Extract screenshot image data if present.
    // Formats:
    //   Action envelope (new): {"screenshot_before": {...}, "screenshot_after": {"data": "...", "format": "webp"}}
    //   Only the after screenshot is sent to MCP clients; before screenshot data is stripped.
    //   Action envelope (legacy): {"screenshot": {"data": "...", "format": "webp", ...}}
    //   Screenshot endpoint: {"data": "...", "mimeType": "image/webp", ...}

    // Helper to extract image data from a screenshot dict and determine mime
    auto extract_image = [](base::Value::Dict* dict, std::string& out_data,
                            std::string& out_mime) {
      if (!dict) return;
      std::string* data = dict->FindString("data");
      if (!data || data->empty()) return;
      out_data = std::move(*data);
      dict->Remove("data");
      out_mime = "image/webp";
      const std::string* format = dict->FindString("format");
      if (format) {
        if (*format == "png") out_mime = "image/png";
        else if (*format == "jpeg") out_mime = "image/jpeg";
      }
    };

    std::string after_data, after_mime;
    std::string single_data, single_mime;

    // Strip before screenshot data from the response (not sent to MCP clients)
    if (auto* before_dict = response_dict.FindDict("screenshot_before")) {
      before_dict->Remove("data");
    }

    // Check new after format
    extract_image(response_dict.FindDict("screenshot_after"),
                  after_data, after_mime);

    // Fallback: legacy single "screenshot" dict
    if (after_data.empty()) {
      extract_image(response_dict.FindDict("screenshot"),
                    single_data, single_mime);
    }

    // Fallback: direct screenshot endpoint format
    if (after_data.empty() && single_data.empty()) {
      std::string* data = response_dict.FindString("data");
      const std::string* top_mime = response_dict.FindString("mimeType");
      if (data && !data->empty()) {
        single_data = std::move(*data);
        response_dict.Remove("data");
        single_mime = top_mime ? *top_mime : "image/webp";
      }
    }

    // Serialize the remaining JSON (without the large base64 data) as text
    std::string pretty_json;
    base::JSONWriter::WriteWithOptions(
        *parsed, base::JSONWriter::OPTIONS_PRETTY_PRINT, &pretty_json);
    base::Value::Dict text_content;
    text_content.Set("type", "text");
    text_content.Set("text", pretty_json);
    content.Append(std::move(text_content));

    // Add after screenshot as image content block
    if (!after_data.empty()) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(after_data));
      img.Set("mimeType", after_mime);
      content.Append(std::move(img));
    }
    // Fallback: single screenshot image
    if (!single_data.empty()) {
      base::Value::Dict img;
      img.Set("type", "image");
      img.Set("data", std::move(single_data));
      img.Set("mimeType", single_mime);
      content.Append(std::move(img));
    }
  } else {
    // Not JSON, return as-is
    base::Value::Dict text_content;
    text_content.Set("type", "text");
    text_content.Set("text", body);
    content.Append(std::move(text_content));
  }

  result.Set("content", std::move(content));

  // If the REST call failed, mark as error
  if (status >= 400) {
    result.Set("isError", true);
  }

  SendJsonRpcResult(std::move(request_id), base::Value(std::move(result)),
                    std::move(callback));
}

void AbpMcpHandler::SendJsonRpcResult(base::Value request_id,
                                      base::Value result,
                                      ResponseWithHeadersCallback callback) {
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));
  response.Set("result", std::move(result));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", {}, response_json);
}

void AbpMcpHandler::SendJsonRpcError(base::Value request_id,
                                     int error_code,
                                     const std::string& message,
                                     ResponseWithHeadersCallback callback) {
  base::Value::Dict response;
  response.Set("jsonrpc", "2.0");
  response.Set("id", std::move(request_id));

  base::Value::Dict error;
  error.Set("code", error_code);
  error.Set("message", message);
  response.Set("error", std::move(error));

  std::string response_json;
  base::JSONWriter::Write(base::Value(std::move(response)), &response_json);

  std::move(callback).Run(200, "application/json", {}, response_json);
}

void AbpMcpHandler::SendAccepted(ResponseWithHeadersCallback callback) {
  std::move(callback).Run(202, "application/json", {}, "");
}

}  // namespace abp
