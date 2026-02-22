// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { createInterface } from "node:readline";
import http from "node:http";
import { launch, type Browser } from "./launch.js";
import { transformMcpResponse } from "./transform.js";

const cliArgs = process.argv.slice(2);
const PORT = parseInt(process.env.ABP_PORT || "8222", 10);
const HEADLESS =
  process.env.ABP_HEADLESS === "1" || cliArgs.includes("--headless");
const EXTRA_ARGS = process.env.ABP_ARGS?.split(",").filter(Boolean) || [];

// Claude's recommended resolution for web applications (WXGA)
// At 1280x800 = 1,024,000 pixels, under Claude's 1.15MP / 1568px limits
const WINDOW_WIDTH = 1280;
const WINDOW_HEIGHT = 800;

let browser: Browser | null = null;
let mcpSessionId: string | null = null;
let launching: Promise<void> | null = null;

function log(msg: string) {
  process.stderr.write(`[abp-proxy] ${msg}\n`);
}

async function ensureBrowser(): Promise<void> {
  if (browser) return;
  if (launching) return launching;

  launching = (async () => {
    // Check if ABP is already running on this port
    try {
      const status = await httpGet(
        `http://localhost:${PORT}/api/v1/browser/status`,
      );
      if (JSON.parse(status)?.data?.ready) {
        log(`Connected to existing ABP on port ${PORT}`);
        launching = null;
        return;
      }
    } catch {
      // Not running, will launch
    }

    log(`Launching ABP on port ${PORT}...`);
    try {
      browser = await launch({
        port: PORT,
        headless: HEADLESS,
        args: [
          `--window-size=${WINDOW_WIDTH},${WINDOW_HEIGHT}`,
          ...EXTRA_ARGS,
        ],
      });
      log(`ABP ready on port ${PORT}`);
    } catch (err: any) {
      log(`Failed to launch ABP: ${err.message}`);
      throw err;
    } finally {
      launching = null;
    }
  })();

  return launching;
}

async function forwardToMcp(message: string): Promise<string> {
  return new Promise((resolve, reject) => {
    const body = Buffer.from(message, "utf-8");
    const headers: Record<string, string> = {
      "Content-Type": "application/json",
      Accept: "application/json, text/event-stream",
      "Content-Length": String(body.length),
    };
    if (mcpSessionId) {
      headers["Mcp-Session-Id"] = mcpSessionId;
    }

    const req = http.request(
      {
        hostname: "localhost",
        port: PORT,
        path: "/mcp",
        method: "POST",
        headers,
        timeout: 60000,
      },
      (res) => {
        // Capture session ID from initialize response
        const sessionHeader = res.headers["mcp-session-id"];
        if (sessionHeader && typeof sessionHeader === "string") {
          mcpSessionId = sessionHeader;
        }

        const chunks: Buffer[] = [];
        res.on("data", (chunk: Buffer) => chunks.push(chunk));
        res.on("end", () => {
          const raw = Buffer.concat(chunks).toString("utf-8");
          if (res.statusCode === 202) {
            // Notification accepted, no response to forward
            resolve("");
          } else {
            resolve(raw);
          }
        });
      },
    );
    req.on("error", reject);
    req.on("timeout", () => {
      req.destroy();
      reject(new Error("MCP request timed out"));
    });
    req.write(body);
    req.end();
  });
}

function httpGet(url: string): Promise<string> {
  return new Promise((resolve, reject) => {
    http
      .get(url, { timeout: 2000 }, (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (c: Buffer) => chunks.push(c));
        res.on("end", () =>
          resolve(Buffer.concat(chunks).toString("utf-8")),
        );
      })
      .on("error", reject)
      .on("timeout", function (this: http.ClientRequest) {
        this.destroy();
        reject(new Error("timeout"));
      });
  });
}

function sendError(id: unknown, code: number, message: string) {
  const error = JSON.stringify({
    jsonrpc: "2.0",
    id,
    error: { code, message },
  });
  process.stdout.write(error + "\n");
}

async function handleMessage(line: string) {
  let msg: any;
  try {
    msg = JSON.parse(line);
  } catch {
    log(`Invalid JSON: ${line.slice(0, 100)}`);
    return;
  }

  const isRequest = "id" in msg;
  const method = msg.method || "";

  // Launch browser on initialize
  if (method === "initialize") {
    try {
      await ensureBrowser();
    } catch (err: any) {
      sendError(msg.id, -32000, `Failed to launch ABP: ${err.message}`);
      return;
    }
  }

  // Forward to ABP's MCP endpoint
  try {
    const responseText = await forwardToMcp(line);
    if (!responseText) return; // notification, no response

    // Transform response (scale images, truncate text)
    const parsed = JSON.parse(responseText);
    const transformed = await transformMcpResponse(parsed);
    process.stdout.write(JSON.stringify(transformed) + "\n");
  } catch (err: any) {
    if (isRequest) {
      sendError(msg.id, -32000, `Proxy error: ${err.message}`);
    } else {
      log(`Error forwarding notification: ${err.message}`);
    }
  }
}

async function shutdown() {
  if (browser) {
    log("Shutting down ABP...");
    await browser.close();
    browser = null;
  }
}

// Main loop: read newline-delimited JSON-RPC from stdin
const rl = createInterface({ input: process.stdin, crlfDelay: Infinity });
rl.on("line", (line) => {
  if (line.trim()) handleMessage(line);
});
rl.on("close", () => shutdown().then(() => process.exit(0)));
process.on("SIGINT", () => shutdown().then(() => process.exit(0)));
process.on("SIGTERM", () => shutdown().then(() => process.exit(0)));
