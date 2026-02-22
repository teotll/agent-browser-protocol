// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { spawn, type ChildProcess } from "node:child_process";
import { ABPClient } from "./client.js";
import { getExecutablePath } from "./paths.js";
import { request } from "./http.js";

export interface LaunchOptions {
  port?: number;
  sessionDir?: string;
  executablePath?: string;
  headless?: boolean;
  /** Window size as [width, height]. Default: [1280, 800]. */
  windowSize?: [number, number];
  args?: string[];
}

export interface Browser {
  client: ABPClient;
  process: ChildProcess;
  port: number;
  close: () => Promise<void>;
}

async function waitForReady(
  baseUrl: string,
  timeoutMs: number = 15000,
): Promise<void> {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const res = await request<{ success: boolean; data: { ready: boolean } }>(
        `${baseUrl}/browser/status`,
        { timeout: 2000 },
      );
      if (res.data?.data?.ready) return;
    } catch {
      // Server not up yet
    }
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
  throw new Error(
    `ABP failed to become ready within ${timeoutMs}ms at ${baseUrl}`,
  );
}

export async function launch(options: LaunchOptions = {}): Promise<Browser> {
  const {
    port = 8222,
    sessionDir,
    executablePath,
    headless = false,
    windowSize,
    args = [],
  } = options;

  const binaryPath = getExecutablePath(executablePath);

  const launchArgs: string[] = [
    `--abp-port=${port}`,
    "--no-first-run",
    "--no-default-browser-check",
    "--use-mock-keychain",
  ];

  if (windowSize) {
    launchArgs.push(`--abp-window-size=${windowSize[0]},${windowSize[1]}`);
  }

  if (sessionDir) {
    launchArgs.push(`--abp-session-dir=${sessionDir}`);
  }

  if (headless) {
    launchArgs.push("--headless=new");
  }

  // Normalize any --headless args to --headless=new (old headless is not supported)
  launchArgs.push(
    ...args.map((a) => (a === "--headless" ? "--headless=new" : a)),
  );

  const child = spawn(binaryPath, launchArgs, {
    stdio: "ignore",
    detached: false,
  });

  const spawnError = new Promise<never>((_, reject) => {
    child.on("error", (err) => {
      reject(new Error(`Failed to launch ABP: ${err.message}`));
    });
  });

  const baseUrl = `http://localhost:${port}/api/v1`;

  try {
    await Promise.race([waitForReady(baseUrl), spawnError]);
  } catch (err) {
    child.kill();
    throw err;
  }

  const client = new ABPClient(baseUrl);

  const close = async (): Promise<void> => {
    try {
      await client.browser.shutdown({ timeout_ms: 5000 });
    } catch {
      // If shutdown fails, force kill
    }
    await new Promise<void>((resolve) => {
      if (child.exitCode !== null) {
        resolve();
        return;
      }
      child.on("exit", () => resolve());
      setTimeout(() => {
        child.kill("SIGKILL");
        resolve();
      }, 5000);
    });
  };

  return { client, process: child, port, close };
}
