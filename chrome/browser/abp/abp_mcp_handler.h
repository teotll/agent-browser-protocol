#ifndef CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_
#define CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_types.h"

namespace abp {

class AbpController;

// Handles MCP Streamable HTTP protocol on /mcp endpoint.
// Implements JSON-RPC 2.0 over HTTP with optional SSE streaming.
//
// Protocol version: 2025-03-26
// Spec: https://modelcontextprotocol.io/specification/2025-06-18/basic/transports
class AbpMcpHandler {
 public:
  explicit AbpMcpHandler(AbpController* controller);
  ~AbpMcpHandler();

  AbpMcpHandler(const AbpMcpHandler&) = delete;
  AbpMcpHandler& operator=(const AbpMcpHandler&) = delete;

  // Handle incoming MCP request
  // method: HTTP method (POST, GET, DELETE)
  // headers: HTTP headers (for session ID, auth, etc.)
  // body: Request body (JSON-RPC message)
  void HandleRequest(const std::string& method,
                     const std::map<std::string, std::string>& headers,
                     const std::string& body,
                     ResponseWithHeadersCallback callback);

 private:
  // JSON-RPC method handlers
  void HandleInitialize(const base::Value::Dict& params,
                        base::Value request_id,
                        ResponseWithHeadersCallback callback);
  void HandleToolsList(base::Value request_id, ResponseWithHeadersCallback callback);
  void HandleToolsCall(const base::Value::Dict& params,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);

  // Tool implementations (delegate to AbpController)
  void CallBrowserGetStatus(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserListTabs(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void CallBrowserNewTab(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserCloseTab(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void CallBrowserGetTabInfo(const base::Value::Dict& args,
                             base::Value request_id,
                             ResponseWithHeadersCallback callback);
  void CallBrowserNavigate(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);
  void CallBrowserGoBack(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserGoForward(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserReload(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserClick(const base::Value::Dict& args,
                        base::Value request_id,
                        ResponseWithHeadersCallback callback);
  void CallBrowserType(const base::Value::Dict& args,
                       base::Value request_id,
                       ResponseWithHeadersCallback callback);
  void CallBrowserScreenshot(const base::Value::Dict& args,
                             base::Value request_id,
                             ResponseWithHeadersCallback callback);
  void CallBrowserExecuteJavascript(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback);
  void CallBrowserKeyboardPress(const base::Value::Dict& args,
                                base::Value request_id,
                                ResponseWithHeadersCallback callback);
  void CallBrowserScroll(const base::Value::Dict& args,
                         base::Value request_id,
                         ResponseWithHeadersCallback callback);
  void CallBrowserMouseMove(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserActivateTab(const base::Value::Dict& args,
                              base::Value request_id,
                              ResponseWithHeadersCallback callback);
  void CallBrowserStopLoading(const base::Value::Dict& args,
                              base::Value request_id,
                              ResponseWithHeadersCallback callback);
  void CallBrowserGetDialog(const base::Value::Dict& args,
                            base::Value request_id,
                            ResponseWithHeadersCallback callback);
  void CallBrowserAcceptDialog(const base::Value::Dict& args,
                               base::Value request_id,
                               ResponseWithHeadersCallback callback);
  void CallBrowserDismissDialog(const base::Value::Dict& args,
                                base::Value request_id,
                                ResponseWithHeadersCallback callback);
  void CallBrowserListDownloads(const base::Value::Dict& args,
                                base::Value request_id,
                                ResponseWithHeadersCallback callback);
  void CallBrowserGetDownload(const base::Value::Dict& args,
                              base::Value request_id,
                              ResponseWithHeadersCallback callback);
  void CallBrowserCancelDownload(const base::Value::Dict& args,
                                 base::Value request_id,
                                 ResponseWithHeadersCallback callback);
  void CallBrowserProvideFiles(const base::Value::Dict& args,
                               base::Value request_id,
                               ResponseWithHeadersCallback callback);
  void CallBrowserKeyboardDown(const base::Value::Dict& args,
                               base::Value request_id,
                               ResponseWithHeadersCallback callback);
  void CallBrowserKeyboardUp(const base::Value::Dict& args,
                             base::Value request_id,
                             ResponseWithHeadersCallback callback);
  void CallBrowserGetExecutionState(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback);
  void CallBrowserSetExecutionState(const base::Value::Dict& args,
                                    base::Value request_id,
                                    ResponseWithHeadersCallback callback);
  void CallBrowserGetText(const base::Value::Dict& args,
                          base::Value request_id,
                          ResponseWithHeadersCallback callback);
  void CallBrowserShutdown(const base::Value::Dict& args,
                           base::Value request_id,
                           ResponseWithHeadersCallback callback);

  // Resolve tab_id from args, falling back to active tab
  std::string ResolveTabId(const base::Value::Dict& args);

  // Response helpers
  void SendJsonRpcResult(base::Value request_id,
                         base::Value result,
                         ResponseWithHeadersCallback callback);
  void SendJsonRpcError(base::Value request_id,
                        int error_code,
                        const std::string& message,
                        ResponseWithHeadersCallback callback);
  void SendAccepted(ResponseWithHeadersCallback callback);

  // Callback adapter: converts AbpController response to MCP tool result
  void OnControllerResponse(base::Value request_id,
                            ResponseWithHeadersCallback callback,
                            int status,
                            const std::string& content_type,
                            std::string body);

  // Controller reference (not owned)
  raw_ptr<AbpController> controller_;

  base::WeakPtrFactory<AbpMcpHandler> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_MCP_HANDLER_H_
