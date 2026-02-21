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

  // 1. browser_action — batched input actions (custom schema with oneOf
  //    discriminated variants per action type; ToolBuilder lacks array/oneOf)
  {
    base::Value::Dict tool;
    tool.Set("name", "browser_action");
    tool.Set("description",
        "Execute 1-3 browser input actions in a single turn. You get one "
        "screenshot back after all actions complete. The page is paused "
        "between your tool calls — JS and animations only run during "
        "execution.\n\n"
        "Batch actions that form a single user intent:\n"
        "- click field + type text + press ENTER  (3 actions, 1 turn)\n"
        "- click field + type text                (2 actions, 1 turn)\n"
        "- standalone click or keypress           (1 action, 1 turn)\n\n"
        "Key names are ALL-CAPS: ENTER, TAB, ESCAPE, ARROWUP, etc.\n"
        "Abbreviations: CTRL, CMD, ESC, DEL, BS, CR, UP, DOWN, LEFT, RIGHT.\n"
        "Modifiers: SHIFT, CONTROL, ALT, META.\n\n"
        "Example — click a search box, type a query, press Enter:\n"
        "{\"actions\":[\n"
        "  {\"type\":\"mouse_click\",\"x\":350,\"y\":200},\n"
        "  {\"type\":\"keyboard_type\",\"text\":\"weather today\"},\n"
        "  {\"type\":\"keyboard_press\",\"key\":\"ENTER\"}\n"
        "]}");

    // -- Shared sub-schemas used across variants --
    base::Value::Dict num_prop;
    num_prop.Set("type", "number");

    base::Value::Dict str_prop;
    str_prop.Set("type", "string");

    // Modifiers array — reused by mouse_click and keyboard_press
    auto make_modifiers = []() {
      base::Value::Dict mods;
      mods.Set("type", "array");
      base::Value::Dict item;
      item.Set("type", "string");
      base::Value::List vals;
      vals.Append("SHIFT");
      vals.Append("CONTROL");
      vals.Append("ALT");
      vals.Append("META");
      item.Set("enum", std::move(vals));
      mods.Set("items", std::move(item));
      return mods;
    };

    // Helper: build a const-type property {"type":"string","const":"value"}
    auto make_const_type = [](const char* value) {
      base::Value::Dict d;
      d.Set("type", "string");
      d.Set("const", value);
      return d;
    };

    // -- oneOf variants --
    base::Value::List one_of;

    // mouse_click: x, y required; button?, click_count?, modifiers? optional
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_click"));
      props.Set("x", num_prop.Clone());
      props.Set("y", num_prop.Clone());

      base::Value::Dict button;
      button.Set("type", "string");
      base::Value::List btn_vals;
      btn_vals.Append("left");
      btn_vals.Append("right");
      btn_vals.Append("middle");
      button.Set("enum", std::move(btn_vals));
      props.Set("button", std::move(button));

      props.Set("click_count", num_prop.Clone());
      props.Set("modifiers", make_modifiers());

      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("x");
      req.Append("y");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // keyboard_type: text required
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("keyboard_type"));
      props.Set("text", str_prop.Clone());
      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("text");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // keyboard_press: key required; modifiers?, action? optional
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("keyboard_press"));
      props.Set("key", str_prop.Clone());
      props.Set("modifiers", make_modifiers());

      base::Value::Dict action_prop;
      action_prop.Set("type", "string");
      base::Value::List action_vals;
      action_vals.Append("press");
      action_vals.Append("down");
      action_vals.Append("up");
      action_prop.Set("enum", std::move(action_vals));
      props.Set("action", std::move(action_prop));

      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("key");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // mouse_hover: x, y required
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_hover"));
      props.Set("x", num_prop.Clone());
      props.Set("y", num_prop.Clone());
      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("x");
      req.Append("y");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // mouse_drag: start_x, start_y, end_x, end_y required; steps? optional
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_drag"));
      props.Set("start_x", num_prop.Clone());
      props.Set("start_y", num_prop.Clone());
      props.Set("end_x", num_prop.Clone());
      props.Set("end_y", num_prop.Clone());
      props.Set("steps", num_prop.Clone());
      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("start_x");
      req.Append("start_y");
      req.Append("end_x");
      req.Append("end_y");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // mouse_slider: slider macro with orientation-discriminated params
    {
      base::Value::Dict variant;
      variant.Set("type", "object");
      base::Value::Dict props;
      props.Set("type", make_const_type("mouse_slider"));

      base::Value::Dict orient_prop;
      orient_prop.Set("type", "string");
      base::Value::List orient_enum;
      orient_enum.Append("horizontal");
      orient_enum.Append("vertical");
      orient_prop.Set("enum", std::move(orient_enum));
      orient_prop.Set("description",
          "Slider orientation. Use 'horizontal' with y, x_start, x_end, "
          "current_x. Use 'vertical' with x, y_start, y_end, current_y.");
      props.Set("orientation", std::move(orient_prop));

      props.Set("y", num_prop.Clone());
      props.Set("x_start", num_prop.Clone());
      props.Set("x_end", num_prop.Clone());
      props.Set("current_x", num_prop.Clone());
      props.Set("x", num_prop.Clone());
      props.Set("y_start", num_prop.Clone());
      props.Set("y_end", num_prop.Clone());
      props.Set("current_y", num_prop.Clone());
      props.Set("min", num_prop.Clone());
      props.Set("max", num_prop.Clone());
      props.Set("target_value", num_prop.Clone());

      variant.Set("properties", std::move(props));
      base::Value::List req;
      req.Append("type");
      req.Append("orientation");
      req.Append("min");
      req.Append("max");
      req.Append("target_value");
      variant.Set("required", std::move(req));
      variant.Set("additionalProperties", false);
      one_of.Append(std::move(variant));
    }

    // -- actions array with oneOf items --
    base::Value::Dict actions_prop;
    actions_prop.Set("type", "array");
    base::Value::Dict items_schema;
    items_schema.Set("oneOf", std::move(one_of));
    actions_prop.Set("items", std::move(items_schema));
    actions_prop.Set("minItems", 1);
    actions_prop.Set("maxItems", 3);

    // -- Top-level properties --
    base::Value::Dict properties;
    properties.Set("actions", std::move(actions_prop));
    properties.Set("tab_id", str_prop.Clone());

    // screenshot config
    base::Value::Dict ss_prop;
    ss_prop.Set("type", "object");
    base::Value::Dict ss_props;

    base::Value::Dict markup_prop;
    markup_prop.Set("type", "array");
    base::Value::Dict markup_item;
    markup_item.Set("type", "string");
    base::Value::List markup_enum;
    markup_enum.Append("clickable");
    markup_enum.Append("typeable");
    markup_enum.Append("scrollable");
    markup_enum.Append("grid");
    markup_enum.Append("selected");
    markup_item.Set("enum", std::move(markup_enum));
    markup_prop.Set("items", std::move(markup_item));
    ss_props.Set("markup", std::move(markup_prop));

    base::Value::Dict disable_prop;
    disable_prop.Set("type", "array");
    base::Value::Dict disable_item;
    disable_item.Set("type", "string");
    disable_prop.Set("items", std::move(disable_item));
    ss_props.Set("disable_markup", std::move(disable_prop));

    base::Value::Dict format_prop;
    format_prop.Set("type", "string");
    base::Value::List format_enum;
    format_enum.Append("png");
    format_enum.Append("webp");
    format_enum.Append("jpeg");
    format_prop.Set("enum", std::move(format_enum));
    ss_props.Set("format", std::move(format_prop));

    ss_prop.Set("properties", std::move(ss_props));
    properties.Set("screenshot", std::move(ss_prop));

    base::Value::Dict schema;
    schema.Set("type", "object");
    schema.Set("properties", std::move(properties));
    base::Value::List required;
    required.Append("actions");
    schema.Set("required", std::move(required));

    tool.Set("inputSchema", std::move(schema));
    tools.Append(std::move(tool));
  }

  // 2. browser_scroll — standalone scroll
  tools.Append(ToolBuilder("browser_scroll")
                   .Description(
                       "Scroll using mouse wheel at element coordinates. "
                       "Simulates moving mouse over element and scrolling. At "
                       "least one of delta_x or delta_y must be non-zero.")
                   .OptionalString("tab_id", "Target tab ID")
                   .RequiredNumber("x",
                       "X pixel coordinate of element center where mouse wheel "
                       "fires. Read from the red grid on your screenshot. Must "
                       "be within viewport bounds.")
                   .RequiredNumber("y",
                       "Y pixel coordinate of element center where mouse wheel "
                       "fires. Read from the red grid on your screenshot. Must "
                       "be within viewport bounds.")
                   .OptionalNumber("delta_x",
                       "Horizontal scroll in pixels (positive=right, "
                       "negative=left, default=0)")
                   .OptionalNumber("delta_y",
                       "Vertical scroll in pixels (negative=up, positive=down, "
                       "default=0)")
                   .Build());

  // 3. browser_navigate — url or back/forward/reload
  tools.Append(ToolBuilder("browser_navigate")
                   .Description(
                       "Navigate to a URL, or go back/forward/reload. "
                       "Provide 'url' to navigate, or 'action' for "
                       "back/forward/reload.")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalString("url", "URL to navigate to")
                   .OptionalStringEnum("action", "Navigation action",
                                       {"back", "forward", "reload"})
                   .Build());

  // 4. browser_screenshot
  tools.Append(
      ToolBuilder("browser_screenshot")
          .Description(
              "Take a screenshot. Also acts as a wait: resumes page "
              "execution, waits for rendering to settle, captures the "
              "viewport, then re-pauses execution. Use this when you need "
              "to let the page load or update before your next action.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalStringArrayEnum("disable_markup",
              "Markup overlays to disable. All overlays are enabled by "
              "default: clickable (green), typeable (orange), scrollable "
              "(purple dashed), grid (red coordinate grid), selected "
              "(blue, focused element)",
              {"clickable", "typeable", "scrollable", "grid", "selected"})
          .OptionalStringArrayEnum("markup",
              "Markup overlays to enable (none by default). clickable "
              "(green), typeable (orange), scrollable (purple dashed), "
              "grid (red coordinate grid), selected (blue, focused element)",
              {"clickable", "typeable", "scrollable", "grid", "selected"})
          .OptionalString("format", "Image format: png, webp, jpeg")
          .Build());

  // 5. browser_tabs — list/new/close/info/activate/stop
  tools.Append(ToolBuilder("browser_tabs")
                   .Description(
                       "Manage browser tabs. Default: list all tabs. "
                       "Actions: list, new (create tab), close, info "
                       "(tab details), activate (switch to tab), stop "
                       "(stop loading).")
                   .OptionalStringEnum("action", "Tab action",
                       {"list", "new", "close", "info", "activate", "stop"})
                   .OptionalString("tab_id",
                       "Target tab ID (for close/info/activate/stop)")
                   .OptionalString("url", "URL for new tab")
                   .Build());

  // 6. browser_javascript
  tools.Append(
      ToolBuilder("browser_javascript")
          .Description(
              "Execute JavaScript in the page context. "
              "WARNING: Do NOT use this as a primary interaction method. "
              "Prefer browser_action (click, type, scroll) for all user "
              "interactions. Only use this tool for: (1) extracting data "
              "from the page (e.g. reading attributes, counting elements), "
              "or (2) locating elements when a mouse/keyboard action failed "
              "to produce the desired result and you need to inspect the DOM "
              "to understand why.")
          .OptionalString("tab_id", "Target tab ID")
          .RequiredString("expression", "JavaScript expression to evaluate")
          .Build());

  // 7. browser_text
  tools.Append(ToolBuilder("browser_text")
                   .Description("Get the visible text content of the page")
                   .OptionalString("tab_id", "Target tab ID")
                   .OptionalString("selector",
                       "CSS selector to scope text extraction")
                   .Build());

  // 8. browser_dialog — check/accept/dismiss
  tools.Append(
      ToolBuilder("browser_dialog")
          .Description(
              "Handle browser dialogs (alert/confirm/prompt). Default: "
              "check if a dialog is pending.")
          .OptionalString("tab_id", "Target tab ID")
          .OptionalStringEnum("action", "Dialog action",
                              {"check", "accept", "dismiss"})
          .OptionalString("prompt_text",
              "Text to enter for prompt dialogs (used with accept)")
          .Build());

  // 9. browser_downloads — list/status/cancel
  tools.Append(
      ToolBuilder("browser_downloads")
          .Description(
              "Manage downloads. Default: list all downloads.")
          .OptionalStringEnum("action", "Download action",
                              {"list", "status", "cancel"})
          .OptionalString("download_id",
              "Download ID (required for status/cancel)")
          .OptionalStringEnum("state", "Filter by download state (for list)",
              {"in_progress", "completed", "cancelled", "failed"})
          .OptionalNumber("limit",
              "Maximum number of downloads to return (for list)")
          .Build());

  // 10. browser_files
  tools.Append(ToolBuilder("browser_files")
                   .Description(
                       "Provide files to a pending file chooser dialog")
                   .RequiredString("chooser_id", "File chooser ID from event")
                   .OptionalStringArray("files", "File paths to provide")
                   .OptionalString("path", "Save path for save dialogs")
                   .OptionalBoolean("cancel", "Cancel the file chooser")
                   .Build());

  // 11. browser_get_status
  tools.Append(ToolBuilder("browser_get_status")
                   .Description("Get browser status and readiness")
                   .Build());

  // 12. browser_shutdown
  tools.Append(ToolBuilder("browser_shutdown")
                   .Description("Gracefully shut down the browser")
                   .OptionalNumber("timeout_ms",
                       "Timeout before force quit in ms")
                   .Build());

  return tools;
}

// Guide content served via resources/read for abp://guide
constexpr char kGuideContent[] = R"md(# ABP Browser Control Guide

## How ABP Works

ABP **pauses JavaScript and virtual time** between your actions. The page is frozen until your next tool call.

When you call any action tool (browser_action, browser_scroll, browser_navigate, etc.):
1. ABP **resumes** JS execution
2. ABP dispatches your action(s)
3. ABP **waits ~500ms** for the page to settle (rendering, network, scripts)
4. ABP captures a **screenshot** automatically
5. ABP **re-pauses** JS execution
6. You receive the response with the screenshot

**One tool call = one complete turn.** Screenshots are included automatically with every action response. There is no need to take a separate screenshot after performing an action.

## Batching Actions

`browser_action` accepts 1-3 actions per call. Batch common workflows to reduce round-trips:

- **Click, type, submit:** `[{mouse_click, x, y}, {keyboard_type, text}, {keyboard_press, key: ENTER}]`
- **Click and type:** `[{mouse_click, x, y}, {keyboard_type, text}]`
- **Single click:** `[{mouse_click, x, y}]`

Actions execute sequentially with a 20ms pause between each. One screenshot is taken after all actions complete.

**Do NOT batch scrolling** — use `browser_scroll` separately.

## Waiting for Slow Content

Sometimes 500ms isn't enough for the page to finish loading (AJAX, animations, redirects). When the screenshot shows incomplete content:

**Call `browser_screenshot` to wait and observe.** It runs the same resume-wait-capture-pause cycle without performing any action, giving the page another chance to settle. Repeat until the content appears.

## Markup Overlays

Pass `markup: ["clickable", "typeable", "grid"]` to `browser_screenshot` to see labeled overlays on interactive elements. Each label shows the element's coordinates for targeting clicks and typing.

## Tool Reference (12 tools)

All `tab_id` parameters are optional and default to the active tab.

**Input:**
- `browser_action` — 1-3 actions: mouse_click (x, y), keyboard_type (text), keyboard_press (key, modifiers?), mouse_hover (x, y), mouse_drag (start_x, start_y, end_x, end_y). Keys are ALL-CAPS (ENTER, TAB, ESCAPE, CONTROL, META, etc.). Abbreviations accepted: CTRL, CMD, ESC, DEL.
- `browser_scroll` — x, y (where wheel fires), delta_x?, delta_y? (positive=down/right)

**Navigation:**
- `browser_navigate` — url? OR action? (back, forward, reload)
- `browser_tabs` — action? (list, new, close, info, activate, stop; default: list), tab_id?, url?

**Observation:**
- `browser_screenshot` — markup?, disable_markup?, format?
- `browser_javascript` — expression (required). Data extraction and DOM inspection ONLY — do NOT use for interaction; prefer browser_action.
- `browser_text` — selector?

**Situational:**
- `browser_dialog` — action? (check, accept, dismiss; default: check), prompt_text?
- `browser_downloads` — action? (list, status, cancel; default: list), download_id?, state?, limit?
- `browser_files` — chooser_id (required), files?, path?, cancel?

**Browser:**
- `browser_get_status` — no params
- `browser_shutdown` — timeout_ms?

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

- `browser_javascript` uses `expression` as its parameter name (not `script`)
- `browser_scroll` requires `x`, `y` coordinates where the mouse wheel fires — target the element center
- Scroll direction: `delta_y` positive = scroll down, negative = scroll up
- JS is paused between actions — timers and animations don't advance until your next tool call
- Key names are ALL-CAPS: ENTER, TAB, ESCAPE, BACKSPACE, ARROWUP, ARROWDOWN, etc.
- Modifier keys for keyboard_press: SHIFT, CONTROL, ALT, META (or abbreviations CTRL, CMD, OPT)
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
  if (*name == "browser_action") {
    CallBrowserAction(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_scroll") {
    CallBrowserScroll(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_navigate") {
    CallBrowserNavigate(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_screenshot") {
    CallBrowserScreenshot(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_tabs") {
    CallBrowserTabs(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_javascript") {
    CallBrowserJavascript(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_text") {
    CallBrowserText(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_dialog") {
    CallBrowserDialog(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_downloads") {
    CallBrowserDownloads(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_files") {
    CallBrowserFiles(*args, std::move(request_id), std::move(callback));
  } else if (*name == "browser_get_status") {
    CallBrowserGetStatus(*args, std::move(request_id), std::move(callback));
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

// --- 1. browser_action: batched input via /batch endpoint ---
void AbpMcpHandler::CallBrowserAction(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  // Forward everything except tab_id to the batch REST endpoint.
  // The body includes "actions" array and optional "screenshot" config.
  base::Value::Dict body_dict = args.Clone();
  body_dict.Remove("tab_id");

  std::string body;
  base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);

  controller_->HandleRequest(
      "POST", "/api/v1/tabs/" + tab_id + "/batch", body,
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 2. browser_scroll: standalone scroll ---
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

// --- 3. browser_navigate: url or back/forward/reload ---
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

  const std::string* url = args.FindString("url");
  const std::string* action = args.FindString("action");

  if (url) {
    base::Value::Dict body_dict;
    body_dict.Set("url", *url);
    std::string body;
    base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/navigate", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (action) {
    if (*action == "back") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/back", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (*action == "forward") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/forward", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (*action == "reload") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/reload", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Invalid action: " + *action +
                           " (expected back, forward, or reload)",
                       std::move(callback));
    }
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Provide 'url' or 'action' (back/forward/reload)",
                     std::move(callback));
  }
}

// --- 4. browser_screenshot ---
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
  if (const base::Value::List* markup = args.FindList("markup")) {
    screenshot_opts.Set("markup", markup->Clone());
  } else if (const base::Value::List* disable_markup =
                 args.FindList("disable_markup")) {
    screenshot_opts.Set("disable_markup", disable_markup->Clone());
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

// --- 5. browser_tabs: list/new/close/info/activate/stop ---
void AbpMcpHandler::CallBrowserTabs(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "list";

  if (act == "list") {
    controller_->HandleRequest(
        "GET", "/api/v1/tabs", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "new") {
    // Reuse the existing new-tab-then-navigate logic.
    const std::string* url = args.FindString("url");
    std::string nav_url = url ? *url : "";

    // Always create tab at about:blank first.  If a URL was requested we
    // follow up with a navigate call so it goes through AbpActionContext
    // (which captures before/after screenshots and waits for page load).
    controller_->HandleRequest(
        "POST", "/api/v1/tabs", R"({"url":"about:blank"})",
        base::BindOnce(
            [](base::WeakPtr<AbpMcpHandler> self, std::string nav_url,
               base::Value request_id, ResponseWithHeadersCallback callback,
               int status, const std::string& content_type,
               std::string body) {
              if (!self)
                return;

              if (nav_url.empty() || nav_url == "about:blank") {
                self->OnControllerResponse(std::move(request_id),
                                           std::move(callback), status,
                                           content_type, std::move(body));
                return;
              }

              auto parsed =
                  base::JSONReader::Read(body, base::JSON_PARSE_RFC);
              std::string tab_id;
              if (parsed && parsed->is_dict()) {
                const std::string* id = parsed->GetDict().FindString("id");
                if (id)
                  tab_id = *id;
              }
              if (tab_id.empty()) {
                self->OnControllerResponse(std::move(request_id),
                                           std::move(callback), status,
                                           content_type, std::move(body));
                return;
              }

              VLOG(1) << "ABP MCP: browser_tabs new - tab created: "
                      << tab_id << ", scheduling navigate to " << nav_url
                      << " in 500ms";
              base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
                  FROM_HERE,
                  base::BindOnce(
                      [](base::WeakPtr<AbpMcpHandler> handler,
                         std::string tid, std::string url,
                         base::Value req_id,
                         ResponseWithHeadersCallback cb) {
                        if (!handler)
                          return;
                        base::Value::Dict nav_body;
                        nav_body.Set("url", url);
                        std::string nav_body_str;
                        base::JSONWriter::Write(
                            base::Value(std::move(nav_body)), &nav_body_str);
                        handler->controller_->HandleRequest(
                            "POST", "/api/v1/tabs/" + tid + "/navigate",
                            nav_body_str,
                            base::BindOnce(
                                &AbpMcpHandler::OnControllerResponse,
                                handler, std::move(req_id), std::move(cb)));
                      },
                      self, std::move(tab_id), std::move(nav_url),
                      std::move(request_id), std::move(callback)),
                  base::Milliseconds(500));
            },
            weak_factory_.GetWeakPtr(), std::move(nav_url),
            std::move(request_id), std::move(callback)));
  } else {
    // close, info, activate, stop all need a tab_id
    std::string tab_id = ResolveTabId(args);
    if (tab_id.empty()) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "No tab_id provided and no active tab available",
                       std::move(callback));
      return;
    }

    if (act == "close") {
      controller_->HandleRequest(
          "DELETE", "/api/v1/tabs/" + tab_id, "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (act == "info") {
      controller_->HandleRequest(
          "GET", "/api/v1/tabs/" + tab_id, "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (act == "activate") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/activate", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else if (act == "stop") {
      controller_->HandleRequest(
          "POST", "/api/v1/tabs/" + tab_id + "/stop", "",
          base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                         weak_factory_.GetWeakPtr(), std::move(request_id),
                         std::move(callback)));
    } else {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Invalid action: " + act +
                           " (expected list, new, close, info, activate, "
                           "or stop)",
                       std::move(callback));
    }
  }
}

// --- 6. browser_javascript ---
void AbpMcpHandler::CallBrowserJavascript(const base::Value::Dict& args,
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

// --- 7. browser_text ---
void AbpMcpHandler::CallBrowserText(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

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

// --- 8. browser_dialog: check/accept/dismiss ---
void AbpMcpHandler::CallBrowserDialog(const base::Value::Dict& args,
                                      base::Value request_id,
                                      ResponseWithHeadersCallback callback) {
  std::string tab_id = ResolveTabId(args);
  if (tab_id.empty()) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "No tab_id provided and no active tab available",
                     std::move(callback));
    return;
  }

  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "check";

  if (act == "check") {
    controller_->HandleRequest(
        "GET", "/api/v1/tabs/" + tab_id + "/dialog", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "accept") {
    base::Value::Dict body_dict;
    if (const std::string* prompt_text = args.FindString("prompt_text")) {
      body_dict.Set("prompt_text", *prompt_text);
    }
    std::string body;
    base::JSONWriter::Write(base::Value(std::move(body_dict)), &body);
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/dialog/accept", body,
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "dismiss") {
    controller_->HandleRequest(
        "POST", "/api/v1/tabs/" + tab_id + "/dialog/dismiss", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action: " + act +
                         " (expected check, accept, or dismiss)",
                     std::move(callback));
  }
}

// --- 9. browser_downloads: list/status/cancel ---
void AbpMcpHandler::CallBrowserDownloads(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  const std::string* action = args.FindString("action");
  std::string act = action ? *action : "list";

  if (act == "list") {
    // Build query string from optional params
    std::string path = "/api/v1/downloads";
    std::vector<std::string> query_parts;

    if (const std::string* state = args.FindString("state")) {
      query_parts.push_back("state=" + *state);
    }
    if (auto limit = args.FindDouble("limit")) {
      query_parts.push_back(
          "limit=" + base::NumberToString(static_cast<int>(*limit)));
    }

    if (!query_parts.empty()) {
      path += "?";
      for (size_t i = 0; i < query_parts.size(); ++i) {
        if (i > 0)
          path += "&";
        path += query_parts[i];
      }
    }

    controller_->HandleRequest(
        "GET", path, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "status") {
    const std::string* download_id = args.FindString("download_id");
    if (!download_id) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Missing download_id for status action",
                       std::move(callback));
      return;
    }
    controller_->HandleRequest(
        "GET", "/api/v1/downloads/" + *download_id, "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else if (act == "cancel") {
    const std::string* download_id = args.FindString("download_id");
    if (!download_id) {
      SendJsonRpcError(std::move(request_id), kInvalidParams,
                       "Missing download_id for cancel action",
                       std::move(callback));
      return;
    }
    controller_->HandleRequest(
        "POST", "/api/v1/downloads/" + *download_id + "/cancel", "",
        base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                       weak_factory_.GetWeakPtr(), std::move(request_id),
                       std::move(callback)));
  } else {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Invalid action: " + act +
                         " (expected list, status, or cancel)",
                     std::move(callback));
  }
}

// --- 10. browser_files ---
void AbpMcpHandler::CallBrowserFiles(const base::Value::Dict& args,
                                     base::Value request_id,
                                     ResponseWithHeadersCallback callback) {
  const std::string* chooser_id = args.FindString("chooser_id");
  if (!chooser_id) {
    SendJsonRpcError(std::move(request_id), kInvalidParams,
                     "Missing chooser_id", std::move(callback));
    return;
  }

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

// --- 11. browser_get_status ---
void AbpMcpHandler::CallBrowserGetStatus(const base::Value::Dict& args,
                                         base::Value request_id,
                                         ResponseWithHeadersCallback callback) {
  controller_->GetBrowserStatus(
      base::BindOnce(&AbpMcpHandler::OnControllerResponse,
                     weak_factory_.GetWeakPtr(), std::move(request_id),
                     std::move(callback)));
}

// --- 12. browser_shutdown ---
void AbpMcpHandler::CallBrowserShutdown(const base::Value::Dict& args,
                                        base::Value request_id,
                                        ResponseWithHeadersCallback callback) {
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
    // REST responses may contain screenshot_before + screenshot_after (action
    // envelope) or a single "screenshot" dict (legacy/screenshot endpoint).
    // For MCP: strip screenshot_before entirely, rename screenshot_after to
    // "screenshot", and emit the image as a native MCP image content block.

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

    // Strip before screenshot entirely from MCP response (not useful to agents)
    response_dict.Remove("screenshot_before");

    // Check new after format and rename to "screenshot" for MCP output
    extract_image(response_dict.FindDict("screenshot_after"),
                  after_data, after_mime);
    if (auto after_val = response_dict.Extract("screenshot_after")) {
      response_dict.Set("screenshot", std::move(*after_val));
    }

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

    // Add screenshot as image content block
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
