// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { spawn, type ChildProcess } from "node:child_process";
import { createServer, type Server } from "node:net";
import { mkdtempSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { ABPClient } from "./client.js";
import { getExecutablePath } from "./paths.js";
import { request } from "./http.js";

/** Default starting port for auto-detection. */
export const DEFAULT_START_PORT = 15678;

/**
 * Test whether a single TCP port is available to bind.
 * Resolves `true` if the port is free, `false` if it is already taken.
 */
function isPortAvailable(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const srv: Server = createServer();
    srv.once("error", () => resolve(false));
    srv.listen(port, "127.0.0.1", () => {
      srv.close(() => resolve(true));
    });
  });
}

/**
 * Find the first available TCP port starting at `startPort` and
 * incrementing by 1 for each port that is already bound.
 * Gives up after 100 attempts.
 */
export async function findAvailablePort(startPort: number = DEFAULT_START_PORT): Promise<number> {
  const maxAttempts = 100;
  for (let i = 0; i < maxAttempts; i++) {
    const candidate = startPort + i;
    if (await isPortAvailable(candidate)) {
      return candidate;
    }
  }
  throw new Error(
    `Could not find an available port after probing ${startPort}–${startPort + maxAttempts - 1}`,
  );
}

export interface LaunchOptions {
  port?: number;
  sessionDir?: string;
  executablePath?: string;
  headless?: boolean;
  /** Window size as [width, height]. Default: [1280, 800]. */
  windowSize?: [number, number];
  /** Pipe browser stdout/stderr to the parent process stderr. */
  verbose?: boolean;
  /** Pre-network settlement wait in ms. Default: 150. */
  minWait?: number;
  /** Request tracking timeout in ms. Default: 1000. */
  trackingTimeout?: number;
  /** Post-network settle time in ms. Default: 350. */
  postSettle?: number;
  /** Chrome user data directory. */
  userDataDir?: string;
  /** Chrome profile directory name (e.g. "Profile 1"). */
  profileDirectory?: string;
  /** Custom User-Agent string. */
  userAgent?: string;
  /** Default zoom factor (e.g. 1.5). Default: 1.0. */
  zoom?: number;
  /** Path to ABP JSON config file. */
  configFile?: string;
  /** Disable execution control (Debugger.pause + virtual time). */
  disablePause?: boolean;
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
    sessionDir,
    executablePath,
    headless = false,
    windowSize,
    verbose = false,
    minWait,
    trackingTimeout,
    postSettle,
    userDataDir,
    profileDirectory,
    userAgent,
    zoom,
    configFile,
    disablePause = false,
    args = [],
  } = options;

  // If no explicit port, probe for the first available one starting at DEFAULT_START_PORT.
  const port = options.port ?? await findAvailablePort();

  const binaryPath = getExecutablePath(executablePath);

  const launchArgs: string[] = [
    `--abp-port=${port}`,
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

  if (minWait !== undefined) {
    launchArgs.push(`--abp-min-wait=${minWait}`);
  }

  if (trackingTimeout !== undefined) {
    launchArgs.push(`--abp-tracking-timeout=${trackingTimeout}`);
  }

  if (postSettle !== undefined) {
    launchArgs.push(`--abp-post-settle=${postSettle}`);
  }

  // Always set --user-data-dir so each launch gets an isolated Chrome instance.
  // Without this, Chrome signals the first instance and exits.
  launchArgs.push(`--user-data-dir=${userDataDir ?? mkdtempSync(join(tmpdir(), "abp-"))}`);

  if (profileDirectory) {
    launchArgs.push(`--profile-directory=${profileDirectory}`);
  }

  if (userAgent) {
    launchArgs.push(`--user-agent=${userAgent}`);
  }

  if (zoom !== undefined) {
    launchArgs.push(`--abp-zoom=${zoom}`);
  }

  if (configFile) {
    launchArgs.push(`--abp-config=${configFile}`);
  }

  if (disablePause) {
    launchArgs.push("--abp-disable-pause");
  }

  // Normalize any --headless args to --headless=new (old headless is not supported)
  launchArgs.push(
    ...args.map((a) => (a === "--headless" ? "--headless=new" : a)),
  );

  const child = spawn(binaryPath, launchArgs, {
    stdio: verbose
      ? ["ignore", process.stderr, process.stderr] as any
      : "ignore",
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
