// mind2web/harness/src/runner.ts
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { AbpHelper } from "./abp.js";
import { createProxyMcpServer } from "./proxy.js";
import { runAgent } from "./agent.js";
import { convertAndSave } from "./converter.js";
import type { BenchmarkConfig, Mind2WebTask, TaskTrajectory } from "./types.js";

export interface TaskResult {
  task_id: string;
  success: boolean;
  steps: number;
  resultPath?: string;
  error?: string;
  durationMs: number;
}

/**
 * Run a single mind2web task end-to-end.
 *
 * Lifecycle:
 *   1. Verify ABP browser is ready
 *   2. Reset browser state (close extra tabs, navigate to about:blank)
 *   3. Navigate to the task's starting URL
 *   4. Take an initial screenshot (step 0)
 *   5. Create the proxy MCP server that records trajectory
 *   6. Run the Claude agent until completion or max turns
 *   7. Convert trajectory to mind2web result format and save
 */
export async function runTask(
  task: Mind2WebTask,
  config: BenchmarkConfig,
): Promise<TaskResult> {
  const startTime = Date.now();
  const taskDir = join(config.outputDir, task.task_id);
  const trajectoryDir = join(taskDir, "trajectory");
  await mkdir(trajectoryDir, { recursive: true });

  const abp = new AbpHelper(config.abpPort);

  try {
    // 1. Verify ABP is ready
    await abp.waitForReady();

    // 2. Reset browser state
    await abp.resetTabs();

    // 3. Navigate to start URL
    const tabId = await abp.getActiveTabId();
    await abp.navigateTo(tabId, task.website);

    // 4. Take initial screenshot (step 0)
    const initialPng = await abp.takeScreenshotPng(tabId);
    await writeFile(join(trajectoryDir, "0_full_screenshot.png"), initialPng);

    // 5. Create proxy MCP server with trajectory recorder
    const { server, state } = await createProxyMcpServer(abp, trajectoryDir);

    // 6. Run the agent.
    // Wire thoughts into proxy state so trajectory entries capture the
    // reasoning that preceded each action.
    const { finalResponse } = await runAgent(task, server, config, (thought) => {
      state.currentThought = thought;
    });

    // 7. Build trajectory and convert to mind2web format
    const trajectory: TaskTrajectory = {
      task,
      entries: state.entries,
      final_response: finalResponse,
    };
    const resultPath = await convertAndSave(trajectory, config.outputDir);

    return {
      task_id: task.task_id,
      success: true,
      steps: state.entries.length,
      resultPath,
      durationMs: Date.now() - startTime,
    };
  } catch (err: unknown) {
    const errorMsg = err instanceof Error ? err.message : String(err);

    // Try to save partial trajectory data even on failure
    try {
      // Access state from the outer scope if proxy was created
      // (it may not exist if the error happened before proxy creation)
      const partialTrajectory: TaskTrajectory = {
        task,
        entries: [], // Will be empty if proxy wasn't created
        final_response: "",
        error: errorMsg,
      };
      await convertAndSave(partialTrajectory, config.outputDir);
    } catch { /* ignore save failures */ }

    return {
      task_id: task.task_id,
      success: false,
      steps: 0,
      error: errorMsg,
      durationMs: Date.now() - startTime,
    };
  }
}
