// mind2web/harness/src/agent.ts
import { query } from "@anthropic-ai/claude-agent-sdk";
import type {
  McpSdkServerConfigWithInstance,
  SDKAssistantMessage,
  SDKResultSuccess,
  SDKMessage,
} from "@anthropic-ai/claude-agent-sdk";
import type { BenchmarkConfig, Mind2WebTask } from "./types.js";

const SYSTEM_PROMPT = `You are a web browsing agent. You interact with web pages using browser tools to accomplish tasks.

Instructions:
- Complete the given task by interacting with the web page
- Use browser_action to click, type, scroll, hover, and drag on the page
- Use browser_screenshot to take a screenshot and observe the current state
- Use browser_navigate to go to URLs or go back/forward/reload
- Use browser_text to read the text content of the page
- Use browser_javascript to inspect page state when needed
- When you believe the task is complete, state what you accomplished
- Do not navigate to Google Search or any other search engine — work within the given website
- Be precise with clicks — aim for the center of interactive elements
- After important actions, take a screenshot to verify the result`;

export { SYSTEM_PROMPT };

/**
 * Run the agent on a single task. Returns the agent's messages
 * so we can extract thoughts and final response.
 */
export async function runAgent(
  task: Mind2WebTask,
  mcpServer: McpSdkServerConfigWithInstance,
  config: BenchmarkConfig,
): Promise<{ thoughts: string[]; finalResponse: string }> {
  const thoughts: string[] = [];
  let finalResponse = "";
  let lastAssistantText = "";

  for await (const message of query({
    prompt: task.confirmed_task,
    options: {
      model: config.model,
      maxTurns: config.maxTurns,
      systemPrompt: SYSTEM_PROMPT,
      mcpServers: {
        browser: mcpServer,
      },
      allowedTools: ["mcp__browser__*"],
      permissionMode: "bypassPermissions",
      allowDangerouslySkipPermissions: true,
      // Disable all built-in tools — we only want the browser MCP tools
      tools: [],
    },
  })) {
    // Capture assistant text blocks (reasoning/thoughts)
    if (message.type === "assistant") {
      const assistantMsg = message as SDKAssistantMessage;
      const content = assistantMsg.message?.content;
      if (Array.isArray(content)) {
        const textBlocks = content.filter(
          (b: any) => b.type === "text",
        );
        if (textBlocks.length) {
          const text = textBlocks
            .map((b: any) => b.text)
            .join("\n");
          lastAssistantText = text;
          thoughts.push(text);
        }
      }
    }

    // Capture final result
    if (message.type === "result" && message.subtype === "success") {
      const resultMsg = message as SDKResultSuccess;
      finalResponse = resultMsg.result || lastAssistantText;
    }
  }

  return { thoughts, finalResponse };
}
