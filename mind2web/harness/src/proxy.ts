// mind2web/harness/src/proxy.ts
import { writeFile, mkdir } from "node:fs/promises";
import { join } from "node:path";
import {
  createSdkMcpServer,
  tool,
} from "@anthropic-ai/claude-agent-sdk";
import type { McpSdkServerConfigWithInstance } from "@anthropic-ai/claude-agent-sdk";
import type { CallToolResult } from "@modelcontextprotocol/sdk/types.js";
import { z } from "zod";
import sharp from "sharp";
import type { AbpHelper } from "./abp.js";
import type { TrajectoryEntry } from "./types.js";

// ---------------------------------------------------------------------------
// Public state shared between the proxy and the harness runner
// ---------------------------------------------------------------------------

export interface ProxyState {
  entries: TrajectoryEntry[];
  stepCounter: number;
  trajectoryDir: string;
  currentThought: string;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Save a PNG screenshot to the trajectory directory and return the path. */
async function saveTrajectoryScreenshot(
  abp: AbpHelper,
  tabId: string,
  state: ProxyState,
  step: number,
): Promise<string> {
  const filename = `${step}_full_screenshot.png`;
  const filepath = join(state.trajectoryDir, filename);
  try {
    const pngBuf = await abp.takeScreenshotPng(tabId);
    await writeFile(filepath, pngBuf);
  } catch (err) {
    // Non-fatal: log and return path anyway (file may be missing)
    console.error(`[proxy] Failed to save trajectory screenshot: ${err}`);
  }
  return filepath;
}

/** Record a trajectory entry and save a screenshot. */
async function recordEntry(
  abp: AbpHelper,
  tabId: string,
  state: ProxyState,
  actionType: string,
  actionDescription: string,
  elementDescriptor: string | null,
): Promise<void> {
  state.stepCounter += 1;
  const step = state.stepCounter;
  const screenshotPath = await saveTrajectoryScreenshot(abp, tabId, state, step);

  const entry: TrajectoryEntry = {
    step,
    action_type: actionType,
    action_description: actionDescription,
    element_descriptor: elementDescriptor,
    thought: state.currentThought,
    screenshot_path: screenshotPath,
    timestamp: Date.now(),
  };
  state.entries.push(entry);
}

/** Get element descriptor at (x, y), returning null on failure. */
async function safeGetElement(
  abp: AbpHelper,
  tabId: string,
  x: number,
  y: number,
): Promise<string | null> {
  try {
    return await abp.getElementAtPoint(tabId, x, y);
  } catch {
    return null;
  }
}

/** Get the active tab ID, returning the first tab. */
async function resolveTabId(
  abp: AbpHelper,
  tabId?: string,
): Promise<string> {
  if (tabId) return tabId;
  return abp.getActiveTabId();
}

// ---------------------------------------------------------------------------
// Action description builders (for mind2web action_history)
// ---------------------------------------------------------------------------

function describeClick(
  element: string | null,
  button?: string,
  clickCount?: number,
): string {
  const el = element || "<unknown>";
  const suffix = button === "right" ? " (right-click)" :
    (clickCount && clickCount > 1) ? ` (${clickCount}x click)` : "";
  return `${el} -> CLICK${suffix}`;
}

function describeType(text: string): string {
  return `Type "${text}"`;
}

function describeKeyPress(key: string, modifiers?: string[]): string {
  const parts = [...(modifiers || []), key];
  return `Press ${parts.join("+")}`;
}

function describeHover(element: string | null): string {
  const el = element || "<unknown>";
  return `${el} -> HOVER`;
}

function describeDrag(
  element: string | null,
  endX: number,
  endY: number,
): string {
  const el = element || "<unknown>";
  return `${el} -> DRAG to (${endX}, ${endY})`;
}

function describeScroll(deltaX: number, deltaY: number): string {
  return `Scroll (delta_x=${deltaX}, delta_y=${deltaY})`;
}

function describeNavigate(url?: string, action?: string): string {
  if (url) return `Navigate to ${url}`;
  if (action === "back") return "Navigate back";
  if (action === "forward") return "Navigate forward";
  if (action === "reload") return "Reload page";
  return "Navigate";
}

// ---------------------------------------------------------------------------
// Tool definitions
// ---------------------------------------------------------------------------

function buildTools(abp: AbpHelper, state: ProxyState) {
  // ---- browser_action ----
  const browserAction = tool(
    "browser_action",
    "Execute 1-3 browser input actions in a single turn. You get one screenshot back after all actions complete. The page is paused between your tool calls — JS and animations only run during execution.\n\nBatch actions that form a single user intent:\n- click field + type text + press ENTER  (3 actions, 1 turn)\n- click field + type text                (2 actions, 1 turn)\n- standalone click or keypress           (1 action, 1 turn)\n\nKey names are ALL-CAPS: ENTER, TAB, ESCAPE, ARROWUP, etc.\nAbbreviations: CTRL, CMD, ESC, DEL, BS, CR, UP, DOWN, LEFT, RIGHT.\nModifiers: SHIFT, CONTROL, ALT, META.\n\nExample — click a search box, type a query, press Enter:\n{\"actions\":[\n  {\"type\":\"mouse_click\",\"x\":350,\"y\":200},\n  {\"type\":\"keyboard_type\",\"text\":\"weather today\"},\n  {\"type\":\"keyboard_press\",\"key\":\"ENTER\"}\n]}",
    {
      actions: z.any().describe("Array of 1-3 input actions"),
      screenshot: z.any().optional().describe("Screenshot options"),
      tab_id: z.string().optional().describe("Target tab ID"),
    },
    async (args): Promise<CallToolResult> => {
      const actions = args.actions as any[];
      const tabId = await resolveTabId(abp, args.tab_id);

      const resultTexts: string[] = [];
      let lastScreenshotData: string | undefined;

      for (const action of actions) {
        const actionType: string = action.type;

        try {
          if (actionType === "mouse_click") {
            const x: number = action.x;
            const y: number = action.y;
            const element = await safeGetElement(abp, tabId, x, y);

            const response = await abp.raw.tabs.click(tabId, {
              x,
              y,
              button: action.button,
              click_count: action.click_count,
              modifiers: action.modifiers,
            });

            lastScreenshotData = response.screenshot_after?.data;
            const desc = describeClick(element, action.button, action.click_count);
            await recordEntry(abp, tabId, state, "click", desc, element);
            resultTexts.push(`Clicked at (${x}, ${y})`);

          } else if (actionType === "keyboard_type") {
            const text: string = action.text;

            const response = await abp.raw.tabs.type(tabId, { text });

            lastScreenshotData = response.screenshot_after?.data;
            const desc = describeType(text);
            await recordEntry(abp, tabId, state, "type", desc, null);
            resultTexts.push(`Typed "${text}"`);

          } else if (actionType === "keyboard_press") {
            const key: string = action.key;
            const modifiers: string[] | undefined = action.modifiers;

            const response = await abp.raw.tabs.keyPress(tabId, {
              key,
              modifiers: modifiers as any,
            });

            lastScreenshotData = response.screenshot_after?.data;
            const desc = describeKeyPress(key, modifiers);
            await recordEntry(abp, tabId, state, "key_press", desc, null);
            resultTexts.push(`Pressed ${key}`);

          } else if (actionType === "mouse_hover") {
            const x: number = action.x;
            const y: number = action.y;
            const element = await safeGetElement(abp, tabId, x, y);

            const response = await abp.raw.tabs.move(tabId, { x, y });

            lastScreenshotData = response.screenshot_after?.data;
            const desc = describeHover(element);
            await recordEntry(abp, tabId, state, "hover", desc, element);
            resultTexts.push(`Hovered at (${x}, ${y})`);

          } else if (actionType === "mouse_drag") {
            const startX: number = action.start_x;
            const startY: number = action.start_y;
            const endX: number = action.end_x;
            const endY: number = action.end_y;
            const element = await safeGetElement(abp, tabId, startX, startY);

            const response = await abp.raw.tabs.drag(tabId, {
              start_x: startX,
              start_y: startY,
              end_x: endX,
              end_y: endY,
              steps: action.steps,
            });

            lastScreenshotData = response.screenshot_after?.data;
            const desc = describeDrag(element, endX, endY);
            await recordEntry(abp, tabId, state, "drag", desc, element);
            resultTexts.push(`Dragged from (${startX}, ${startY}) to (${endX}, ${endY})`);

          } else {
            resultTexts.push(`Unknown action type: ${actionType}`);
          }
        } catch (err) {
          const errMsg = err instanceof Error ? err.message : String(err);
          resultTexts.push(`Error executing ${actionType}: ${errMsg}`);
        }
      }

      // Take a final screenshot with markup overlays for the agent
      const content: CallToolResult["content"] = [];
      content.push({ type: "text" as const, text: resultTexts.join("\n") });

      try {
        const screenshotResponse = await abp.raw.tabs.screenshot(tabId, {});
        const screenshotData = screenshotResponse.screenshot_after?.data;
        if (screenshotData) {
          content.push({
            type: "image" as const,
            data: screenshotData,
            mimeType: "image/webp",
          });
        }
      } catch (err) {
        content.push({
          type: "text" as const,
          text: `[Screenshot failed: ${err instanceof Error ? err.message : String(err)}]`,
        });
      }

      return { content };
    },
  );

  // ---- browser_screenshot ----
  const browserScreenshot = tool(
    "browser_screenshot",
    "Take a screenshot of the current page. Also acts as a wait: resumes page execution, waits for rendering to settle, captures the viewport, then re-pauses execution.",
    {
      disable_markup: z.any().optional().describe("Markup overlays to disable"),
      markup: z.any().optional().describe("Markup overlays to enable"),
      format: z.string().optional().describe("Image format: png, webp, jpeg"),
      tab_id: z.string().optional().describe("Target tab ID"),
    },
    async (args): Promise<CallToolResult> => {
      const tabId = await resolveTabId(abp, args.tab_id);

      try {
        const response = await abp.raw.tabs.screenshot(tabId, {
          disable_markup: args.disable_markup,
          format: args.format,
        });

        const data = response.screenshot_after?.data;
        if (!data) {
          return { content: [{ type: "text", text: "Screenshot captured but no image data returned" }] };
        }

        return {
          content: [{
            type: "image" as const,
            data,
            mimeType: "image/webp",
          }],
        };
      } catch (err) {
        return {
          content: [{ type: "text", text: `Screenshot failed: ${err instanceof Error ? err.message : String(err)}` }],
          isError: true,
        };
      }
    },
  );

  // ---- browser_navigate ----
  const browserNavigate = tool(
    "browser_navigate",
    "Navigate to a URL, or go back/forward/reload. Provide 'url' to navigate, or 'action' for back/forward/reload.",
    {
      url: z.string().optional().describe("URL to navigate to"),
      action: z.enum(["back", "forward", "reload"]).optional().describe("Navigation action"),
      tab_id: z.string().optional().describe("Target tab ID"),
    },
    async (args): Promise<CallToolResult> => {
      const tabId = await resolveTabId(abp, args.tab_id);

      try {
        if (args.url) {
          await abp.raw.tabs.navigate(tabId, { url: args.url });
          const desc = describeNavigate(args.url);
          await recordEntry(abp, tabId, state, "navigate", desc, null);

          // Take screenshot with markup for the agent
          try {
            const screenshotResponse = await abp.raw.tabs.screenshot(tabId, {});
            const screenshotData = screenshotResponse.screenshot_after?.data;
            if (screenshotData) {
              return {
                content: [
                  { type: "text" as const, text: `Navigated to ${args.url}` },
                  { type: "image" as const, data: screenshotData, mimeType: "image/webp" },
                ],
              };
            }
          } catch { /* fall through to text-only */ }

          return { content: [{ type: "text", text: `Navigated to ${args.url}` }] };

        } else if (args.action === "back") {
          await abp.raw.tabs.back(tabId);
          const desc = describeNavigate(undefined, "back");
          await recordEntry(abp, tabId, state, "navigate_back", desc, null);
          return { content: [{ type: "text", text: "Navigated back" }] };

        } else if (args.action === "forward") {
          await abp.raw.tabs.forward(tabId);
          const desc = describeNavigate(undefined, "forward");
          await recordEntry(abp, tabId, state, "navigate_forward", desc, null);
          return { content: [{ type: "text", text: "Navigated forward" }] };

        } else if (args.action === "reload") {
          await abp.raw.tabs.reload(tabId);
          const desc = describeNavigate(undefined, "reload");
          await recordEntry(abp, tabId, state, "reload", desc, null);
          return { content: [{ type: "text", text: "Reloaded page" }] };

        } else {
          return {
            content: [{ type: "text", text: "Must provide either 'url' or 'action'" }],
            isError: true,
          };
        }
      } catch (err) {
        return {
          content: [{ type: "text", text: `Navigation failed: ${err instanceof Error ? err.message : String(err)}` }],
          isError: true,
        };
      }
    },
  );

  // ---- browser_scroll ----
  const browserScroll = tool(
    "browser_scroll",
    "Scroll using mouse wheel at element coordinates. Simulates moving mouse over element and scrolling. At least one of delta_x or delta_y must be non-zero.",
    {
      x: z.number().describe("X pixel coordinate where mouse wheel fires"),
      y: z.number().describe("Y pixel coordinate where mouse wheel fires"),
      delta_x: z.number().optional().describe("Horizontal scroll in pixels (positive=right, negative=left, default=0)"),
      delta_y: z.number().optional().describe("Vertical scroll in pixels (negative=up, positive=down, default=0)"),
      tab_id: z.string().optional().describe("Target tab ID"),
    },
    async (args): Promise<CallToolResult> => {
      const tabId = await resolveTabId(abp, args.tab_id);
      const deltaX = args.delta_x ?? 0;
      const deltaY = args.delta_y ?? 0;

      try {
        await abp.raw.tabs.scroll(tabId, {
          x: args.x,
          y: args.y,
          delta_x: deltaX,
          delta_y: deltaY,
        });

        const desc = describeScroll(deltaX, deltaY);
        await recordEntry(abp, tabId, state, "scroll", desc, null);

        // Take screenshot with markup for the agent
        try {
          const screenshotResponse = await abp.raw.tabs.screenshot(tabId, {});
          const screenshotData = screenshotResponse.screenshot_after?.data;
          if (screenshotData) {
            return {
              content: [
                { type: "text" as const, text: `Scrolled at (${args.x}, ${args.y}) with delta (${deltaX}, ${deltaY})` },
                { type: "image" as const, data: screenshotData, mimeType: "image/webp" },
              ],
            };
          }
        } catch { /* fall through to text-only */ }

        return {
          content: [{ type: "text", text: `Scrolled at (${args.x}, ${args.y}) with delta (${deltaX}, ${deltaY})` }],
        };
      } catch (err) {
        return {
          content: [{ type: "text", text: `Scroll failed: ${err instanceof Error ? err.message : String(err)}` }],
          isError: true,
        };
      }
    },
  );

  // ---- browser_text ----
  const browserText = tool(
    "browser_text",
    "Get the visible text content of the page.",
    {
      selector: z.string().optional().describe("CSS selector to scope text extraction"),
      tab_id: z.string().optional().describe("Target tab ID"),
    },
    async (args): Promise<CallToolResult> => {
      const tabId = await resolveTabId(abp, args.tab_id);

      try {
        const result = await abp.raw.tabs.text(tabId, {
          selector: args.selector,
        });

        return {
          content: [{ type: "text", text: result.text ?? "(empty page)" }],
        };
      } catch (err) {
        return {
          content: [{ type: "text", text: `Text extraction failed: ${err instanceof Error ? err.message : String(err)}` }],
          isError: true,
        };
      }
    },
  );

  // ---- browser_javascript ----
  const browserJavascript = tool(
    "browser_javascript",
    "Execute JavaScript in the page context. Only use this for extracting data from the page or locating elements when a mouse/keyboard action failed.",
    {
      expression: z.string().describe("JavaScript expression to evaluate"),
      tab_id: z.string().optional().describe("Target tab ID"),
    },
    async (args): Promise<CallToolResult> => {
      const tabId = await resolveTabId(abp, args.tab_id);

      try {
        const response = await abp.raw.tabs.execute(tabId, {
          script: args.expression,
        });

        const value = response.result?.value;
        const text = value === undefined ? "undefined" : JSON.stringify(value, null, 2);

        return {
          content: [{ type: "text", text }],
        };
      } catch (err) {
        return {
          content: [{ type: "text", text: `JS execution failed: ${err instanceof Error ? err.message : String(err)}` }],
          isError: true,
        };
      }
    },
  );

  // ---- browser_tabs ----
  const browserTabs = tool(
    "browser_tabs",
    "Manage browser tabs. Default: list all tabs. Actions: list, new (create tab), close, info (tab details), activate (switch to tab), stop (stop loading).",
    {
      action: z.enum(["list", "new", "close", "info", "activate", "stop"]).optional().describe("Tab action"),
      tab_id: z.string().optional().describe("Target tab ID (for close/info/activate/stop)"),
      url: z.string().optional().describe("URL for new tab"),
    },
    async (args): Promise<CallToolResult> => {
      const action = args.action || "list";

      try {
        if (action === "list") {
          const tabs = await abp.raw.tabs.list();
          return {
            content: [{ type: "text", text: JSON.stringify(tabs, null, 2) }],
          };

        } else if (action === "new") {
          const created = await abp.raw.tabs.create({ url: args.url });
          return {
            content: [{ type: "text", text: JSON.stringify(created, null, 2) }],
          };

        } else if (action === "close") {
          if (!args.tab_id) {
            return { content: [{ type: "text", text: "tab_id required for close action" }], isError: true };
          }
          await abp.raw.tabs.close(args.tab_id);
          return {
            content: [{ type: "text", text: `Closed tab ${args.tab_id}` }],
          };

        } else if (action === "info") {
          const tabId = args.tab_id || await abp.getActiveTabId();
          const info = await abp.raw.tabs.get(tabId);
          return {
            content: [{ type: "text", text: JSON.stringify(info, null, 2) }],
          };

        } else if (action === "activate") {
          if (!args.tab_id) {
            return { content: [{ type: "text", text: "tab_id required for activate action" }], isError: true };
          }
          const result = await abp.raw.tabs.activate(args.tab_id);
          return {
            content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
          };

        } else if (action === "stop") {
          const tabId = args.tab_id || await abp.getActiveTabId();
          const result = await abp.raw.tabs.stop(tabId);
          return {
            content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
          };

        } else {
          return {
            content: [{ type: "text", text: `Unknown tab action: ${action}` }],
            isError: true,
          };
        }
      } catch (err) {
        return {
          content: [{ type: "text", text: `Tab operation failed: ${err instanceof Error ? err.message : String(err)}` }],
          isError: true,
        };
      }
    },
  );

  return [
    browserAction,
    browserScreenshot,
    browserNavigate,
    browserScroll,
    browserText,
    browserJavascript,
    browserTabs,
  ];
}

// ---------------------------------------------------------------------------
// Public factory
// ---------------------------------------------------------------------------

/**
 * Create the proxy MCP server that intercepts agent tool calls,
 * captures element descriptors, forwards to ABP, and records trajectory.
 */
export async function createProxyMcpServer(
  abp: AbpHelper,
  trajectoryDir: string,
): Promise<{ server: McpSdkServerConfigWithInstance; state: ProxyState }> {
  // Ensure the trajectory directory exists
  await mkdir(trajectoryDir, { recursive: true });

  const state: ProxyState = {
    entries: [],
    stepCounter: 0,
    trajectoryDir,
    currentThought: "",
  };

  const tools = buildTools(abp, state);

  const server = createSdkMcpServer({
    name: "browser",
    version: "1.0.0",
    tools,
  });

  return { server, state };
}
