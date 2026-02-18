#!/usr/bin/env node

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { spawn, type ChildProcess } from "node:child_process";
import { fileURLToPath } from "node:url";
import Database from "better-sqlite3";
import { getExecutablePath } from "../paths.js";

// --- CLI Arg Parsing ---

interface DebugArgs {
  port: number;
  abpUrl: string;
  sessionDir: string; // only used as default for launching ABP
  abpBinary: string;
}

function findAbpBinary(explicit?: string): string {
  // 1. Explicitly provided path
  if (explicit && fs.existsSync(explicit)) return explicit;
  // 2. ABP_BROWSER_PATH env var
  if (process.env.ABP_BROWSER_PATH && fs.existsSync(process.env.ABP_BROWSER_PATH)) {
    return process.env.ABP_BROWSER_PATH;
  }
  // 3. getExecutablePath() from paths.ts (browsers/ dir for installed package)
  try {
    const p = getExecutablePath();
    if (fs.existsSync(p)) return p;
  } catch { /* not found */ }
  // 4. Common dev build locations relative to cwd
  const devPaths = [
    "out/Default/ABP.app/Contents/MacOS/ABP",
    "../out/Default/ABP.app/Contents/MacOS/ABP",
    "../../out/Default/ABP.app/Contents/MacOS/ABP",
  ];
  for (const rel of devPaths) {
    const p = path.resolve(process.cwd(), rel);
    if (fs.existsSync(p)) return p;
  }
  return "";
}

function parseArgs(argv: string[]): DebugArgs {
  let port = 8223;
  let abpUrl = "http://localhost:8222";
  let sessionDir = "";
  let abpBinary = "";

  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    if ((arg === "--port" || arg === "-p") && i + 1 < argv.length) {
      port = parseInt(argv[++i], 10);
    } else if (arg.startsWith("--port=")) {
      port = parseInt(arg.split("=")[1], 10);
    } else if (arg === "--abp-url" && i + 1 < argv.length) {
      abpUrl = argv[++i];
    } else if (arg.startsWith("--abp-url=")) {
      abpUrl = arg.split("=").slice(1).join("=");
    } else if (arg === "--session-dir" && i + 1 < argv.length) {
      sessionDir = argv[++i];
    } else if (arg.startsWith("--session-dir=")) {
      sessionDir = arg.split("=").slice(1).join("=");
    } else if (arg === "--abp-binary" && i + 1 < argv.length) {
      abpBinary = argv[++i];
    } else if (arg.startsWith("--abp-binary=")) {
      abpBinary = arg.split("=").slice(1).join("=");
    } else if (arg === "--help" || arg === "-h") {
      console.log(`abp-debug — ABP Debug Server

Usage:
  abp-debug [options]

Options:
  --session-dir <path>   Session directory for launching ABP (default: sessions/<timestamp>)
  --abp-binary <path>    Path to ABP browser binary (auto-detected)
  --port <port>          Debug server port (default: 8223)
  --abp-url <url>        ABP base URL (default: http://localhost:8222)
  --help, -h             Show this help message

Session directory is always auto-detected from the running ABP instance.
--session-dir is only used when launching ABP via /control/start.`);
      process.exit(0);
    } else {
      console.error(`Unknown option: ${arg}\nRun with --help for usage.`);
      process.exit(1);
    }
  }

  if (!sessionDir) {
    const ts = new Date().toISOString().replace(/[:\-T]/g, "").slice(0, 15);
    sessionDir = path.join(process.cwd(), "sessions", ts);
  }

  abpBinary = findAbpBinary(abpBinary);

  return { port, abpUrl: abpUrl.replace(/\/+$/, ""), sessionDir: path.resolve(sessionDir), abpBinary };
}

// --- SQLite ---

function getSession(db: Database.Database): Record<string, unknown> | null {
  const row = db.prepare(
    "SELECT id, start_time, end_time, browser_version, user_agent FROM sessions ORDER BY start_time DESC LIMIT 1"
  ).get() as Record<string, unknown> | undefined;
  return row || null;
}

function getActions(db: Database.Database, sessionId: string): Record<string, unknown>[] {
  return db.prepare(
    `SELECT id, session_id, tab_id, action_type, timestamp, duration_ms,
            params, result, success, error_message,
            screenshot_before_path, screenshot_after_path
     FROM actions WHERE session_id = ? ORDER BY id DESC`
  ).all(sessionId) as Record<string, unknown>[];
}

function getAction(db: Database.Database, actionId: string): Record<string, unknown> | null {
  const row = db.prepare(
    `SELECT id, session_id, tab_id, action_type, timestamp, duration_ms,
            params, result, success, error_message,
            screenshot_before_path, screenshot_after_path
     FROM actions WHERE id = ?`
  ).get(actionId) as Record<string, unknown> | undefined;
  return row || null;
}

function getMaxActionId(db: Database.Database, sessionId: string): number {
  const row = db.prepare(
    "SELECT MAX(id) as max_id, COUNT(*) as count FROM actions WHERE session_id = ?"
  ).get(sessionId) as { max_id: number | null; count: number };
  return row.max_id || 0;
}

// --- Fetch Session Data from running ABP ---

interface AbpSessionData {
  session_dir: string;
  database_path: string;
  screenshots_dir: string;
}

async function fetchAbpSessionData(abpUrl: string): Promise<AbpSessionData | null> {
  return new Promise((resolve) => {
    const url = new URL("/api/v1/browser/session-data", abpUrl);
    const req = http.get(url, { timeout: 3000, family: 4 }, (res) => {
      let body = "";
      res.on("data", (chunk: Buffer) => { body += chunk; });
      res.on("end", () => {
        try {
          const json = JSON.parse(body);
          if (json.success && json.data?.session_dir) {
            resolve({
              session_dir: json.data.session_dir,
              database_path: json.data.database_path,
              screenshots_dir: json.data.screenshots_dir,
            });
          } else {
            resolve(null);
          }
        } catch {
          resolve(null);
        }
      });
    });
    req.on("error", () => resolve(null));
    req.on("timeout", () => { req.destroy(); resolve(null); });
  });
}

// --- API Proxy ---

function proxyToAbp(
  abpUrl: string,
  req: http.IncomingMessage,
  res: http.ServerResponse,
): void {
  const targetUrl = new URL(req.url || "/", abpUrl);
  const chunks: Buffer[] = [];

  req.on("data", (chunk: Buffer) => chunks.push(chunk));
  req.on("end", () => {
    const body = Buffer.concat(chunks);
    const proxyReq = http.request(
      targetUrl,
      {
        method: req.method,
        headers: {
          ...req.headers,
          host: targetUrl.host,
        },
        timeout: 60000,
        family: 4,
      },
      (proxyRes) => {
        res.writeHead(proxyRes.statusCode || 500, proxyRes.headers);
        proxyRes.pipe(res);
      },
    );
    proxyReq.on("error", (err) => {
      res.writeHead(502, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: `Proxy error: ${err.message}` }));
    });
    proxyReq.on("timeout", () => {
      proxyReq.destroy();
      res.writeHead(504, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "Proxy timeout" }));
    });
    if (body.length > 0) proxyReq.write(body);
    proxyReq.end();
  });
}

// --- SSE ---

const sseClients = new Set<http.ServerResponse>();

function broadcastSSE(data: string): void {
  for (const client of sseClients) {
    client.write(`data: ${data}\n\n`);
  }
}

// --- ABP Process Management ---

let abpProcess: ChildProcess | null = null;

function getAbpPort(abpUrl: string): number {
  try {
    return parseInt(new URL(abpUrl).port, 10) || 8222;
  } catch {
    return 8222;
  }
}

async function checkAbpStatus(abpUrl: string): Promise<boolean> {
  return new Promise((resolve) => {
    const url = new URL("/api/v1/browser/status", abpUrl);
    // Force IPv4 — ABP listens on 127.0.0.1, not ::1
    const req = http.get(url, { timeout: 2000, family: 4 }, (res) => {
      let body = "";
      res.on("data", (chunk: Buffer) => { body += chunk; });
      res.on("end", () => {
        try {
          const data = JSON.parse(body);
          resolve(data.success === true);
        } catch {
          resolve(false);
        }
      });
    });
    req.on("error", () => resolve(false));
    req.on("timeout", () => { req.destroy(); resolve(false); });
  });
}

interface LaunchConfig {
  sessionDir: string;
  executablePath?: string;
  windowWidth?: number;
  windowHeight?: number;
  headless?: boolean;
  extraArgs?: string[];
}

async function startAbp(abpUrl: string, config: LaunchConfig): Promise<{ ok: boolean; error?: string }> {
  if (abpProcess && abpProcess.exitCode === null) {
    return { ok: false, error: "ABP process already running" };
  }

  // Check if ABP is already reachable (started externally)
  if (await checkAbpStatus(abpUrl)) {
    return { ok: false, error: "ABP is already running at " + abpUrl };
  }

  const port = getAbpPort(abpUrl);
  const binaryPath = config.executablePath;
  if (!binaryPath) {
    return { ok: false, error: "ABP binary not found. Set --abp-binary, ABP_BROWSER_PATH, or specify in launch config." };
  }
  if (!fs.existsSync(binaryPath)) {
    return { ok: false, error: `ABP binary not found at: ${binaryPath}` };
  }

  // Ensure session dir exists
  const sessionDir = config.sessionDir;
  if (!fs.existsSync(sessionDir)) {
    fs.mkdirSync(sessionDir, { recursive: true });
  }

  const launchArgs = [
    `--abp-port=${port}`,
    `--abp-session-dir=${sessionDir}`,
    "--no-first-run",
    "--no-default-browser-check",
  ];

  const w = config.windowWidth || 1280;
  const h = config.windowHeight || 800;
  launchArgs.push(`--abp-window-size=${w},${h}`);

  if (config.headless) {
    launchArgs.push("--headless=new");
  }

  if (config.extraArgs && config.extraArgs.length > 0) {
    launchArgs.push(
      ...config.extraArgs.map((a) => (a === "--headless" ? "--headless=new" : a)),
    );
  }

  console.log(`Starting ABP: ${binaryPath}`);
  console.log(`  Args: ${launchArgs.join(" ")}`);

  try {
    abpProcess = spawn(binaryPath, launchArgs, {
      stdio: ["ignore", "ignore", "pipe"],
      detached: false,
    });

    let stderrOutput = "";
    if (abpProcess.stderr) {
      abpProcess.stderr.on("data", (chunk: Buffer) => {
        stderrOutput += chunk.toString();
      });
    }

    abpProcess.on("error", (err) => {
      console.error(`ABP process error: ${err.message}`);
      abpProcess = null;
    });

    abpProcess.on("exit", (code) => {
      console.log(`ABP process exited with code ${code}`);
      abpProcess = null;
      broadcastSSE(JSON.stringify({ type: "abp_status", running: false }));
    });

    // Wait for ABP to become ready (up to 15s)
    const start = Date.now();
    while (Date.now() - start < 15000) {
      // Process already exited — don't wait the full timeout
      if (!abpProcess || abpProcess.exitCode !== null) {
        const hint = stderrOutput.trim().slice(0, 200);
        return { ok: false, error: "ABP process exited immediately" + (hint ? ": " + hint : "") };
      }
      if (await checkAbpStatus(abpUrl)) {
        broadcastSSE(JSON.stringify({ type: "abp_status", running: true }));
        return { ok: true };
      }
      await new Promise((r) => setTimeout(r, 300));
    }

    // Timed out
    if (abpProcess) {
      abpProcess.kill();
      abpProcess = null;
    }
    const hint = stderrOutput.trim().slice(0, 200);
    return { ok: false, error: "ABP failed to start within 15 seconds" + (hint ? ": " + hint : "") };
  } catch (err) {
    return { ok: false, error: `Failed to spawn ABP: ${(err as Error).message}` };
  }
}

async function stopAbp(abpUrl: string): Promise<{ ok: boolean; error?: string }> {
  // Try graceful shutdown via API first
  try {
    await new Promise<void>((resolve, reject) => {
      const url = new URL("/api/v1/browser/shutdown", abpUrl);
      const req = http.request(url, { method: "POST", timeout: 5000, family: 4 }, (res) => {
        res.resume();
        res.on("end", () => resolve());
      });
      req.on("error", () => reject());
      req.on("timeout", () => { req.destroy(); reject(); });
      req.end(JSON.stringify({ timeout_ms: 5000 }));
    });
  } catch {
    // API not reachable, try killing process directly
  }

  if (abpProcess && abpProcess.exitCode === null) {
    abpProcess.kill("SIGTERM");
    // Wait up to 5s for exit
    await new Promise<void>((resolve) => {
      const timeout = setTimeout(() => {
        if (abpProcess && abpProcess.exitCode === null) {
          abpProcess.kill("SIGKILL");
        }
        resolve();
      }, 5000);
      if (abpProcess) {
        abpProcess.on("exit", () => { clearTimeout(timeout); resolve(); });
      } else {
        clearTimeout(timeout);
        resolve();
      }
    });
    abpProcess = null;
  }

  broadcastSSE(JSON.stringify({ type: "abp_status", running: false }));
  return { ok: true };
}

function readJsonBody(req: http.IncomingMessage): Promise<Record<string, unknown>> {
  return new Promise((resolve) => {
    const chunks: Buffer[] = [];
    req.on("data", (chunk: Buffer) => chunks.push(chunk));
    req.on("end", () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString()));
      } catch {
        resolve({});
      }
    });
  });
}

// --- HTML ---

function getHtmlPath(): string {
  try {
    // Built file is at dist/bin/debug.js, HTML is at src/debug-ui.html
    const dir = path.dirname(fileURLToPath(import.meta.url));
    return path.resolve(dir, "..", "..", "src", "debug-ui.html");
  } catch {
    return path.resolve(__dirname, "..", "..", "src", "debug-ui.html");
  }
}

// --- Main ---

async function main() {
  const args = parseArgs(process.argv);
  let currentSessionDir = "";
  let db: Database.Database | null = null;
  let sessionId = "";
  let lastMaxId = 0;
  let watcher: fs.FSWatcher | null = null;
  let debounceTimer: ReturnType<typeof setTimeout> | null = null;
  let lastAbpRunning = false;

  function openSessionDb(sessionDir: string): boolean {
    const dbPath = path.join(sessionDir, "history.db");
    if (!fs.existsSync(dbPath)) return false;
    try {
      if (db) db.close();
    } catch { /* ignore */ }
    db = new Database(dbPath, { readonly: true });
    db.pragma("busy_timeout = 5000");
    const session = getSession(db);
    if (!session) return false;
    sessionId = session.id as string;
    lastMaxId = getMaxActionId(db, sessionId);
    currentSessionDir = sessionDir;

    // Restart watcher on new dir
    if (watcher) watcher.close();
    watcher = fs.watch(sessionDir, { recursive: true }, () => {
      if (debounceTimer) clearTimeout(debounceTimer);
      debounceTimer = setTimeout(() => {
        try {
          if (!db) return;
          const newMaxId = getMaxActionId(db, sessionId);
          if (newMaxId > lastMaxId) {
            lastMaxId = newMaxId;
            broadcastSSE(JSON.stringify({ type: "refresh", maxId: newMaxId }));
          }
        } catch {
          // DB might be briefly locked during write
        }
      }, 200);
    });
    return true;
  }

  // Single code path: always auto-detect session dir from running ABP
  async function attachToAbp(): Promise<boolean> {
    const sessionData = await fetchAbpSessionData(args.abpUrl);
    if (!sessionData) return false;
    const opened = openSessionDb(sessionData.session_dir);
    if (opened) {
      broadcastSSE(JSON.stringify({ type: "session_changed", session_dir: currentSessionDir }));
    }
    return opened;
  }

  // Try to attach to a running ABP instance
  const attached = await attachToAbp();
  if (!attached) {
    console.log("Note: No running ABP found. Start ABP to connect.");
  }

  // Load HTML
  const htmlPath = getHtmlPath();
  if (!fs.existsSync(htmlPath)) {
    console.error(`Error: UI file not found: ${htmlPath}`);
    process.exit(1);
  }
  const html = fs.readFileSync(htmlPath, "utf-8");

  // HTTP server
  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url || "/", `http://localhost:${args.port}`);
    const pathname = url.pathname;

    // --- Static ---
    if (req.method === "GET" && pathname === "/") {
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
      res.end(html);
      return;
    }

    // --- SSE ---
    if (req.method === "GET" && pathname === "/events") {
      res.writeHead(200, {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        Connection: "keep-alive",
      });
      sseClients.add(res);
      req.on("close", () => sseClients.delete(res));
      return;
    }

    // --- Data endpoints (SQLite) ---
    if (req.method === "GET" && pathname === "/data/session") {
      // Try to attach if not yet connected
      if (!db) {
        await attachToAbp();
      }
      const session = db ? getSession(db) : null;
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({
        session,
        session_dir: currentSessionDir || null,
        abp_url: args.abpUrl,
        action_count: db && sessionId ? getMaxActionId(db, sessionId) : 0,
      }));
      return;
    }

    if (req.method === "GET" && pathname === "/data/actions") {
      if (!db || !sessionId) {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end("[]");
        return;
      }
      const actions = getActions(db, sessionId);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(actions));
      return;
    }

    const actionMatch = pathname.match(/^\/data\/actions\/(\d+)$/);
    if (req.method === "GET" && actionMatch) {
      if (!db) {
        res.writeHead(404, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "No database" }));
        return;
      }
      const action = getAction(db, actionMatch[1]);
      if (!action) {
        res.writeHead(404, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "Action not found" }));
        return;
      }
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(action));
      return;
    }

    const screenshotMatch = pathname.match(/^\/data\/screenshots\/(.+)$/);
    if (req.method === "GET" && screenshotMatch) {
      const filename = decodeURIComponent(screenshotMatch[1]);
      const screenshotPath = path.join(currentSessionDir, "screenshots", filename);
      const allowedDir = path.join(currentSessionDir, "screenshots") + path.sep;
      if (!screenshotPath.startsWith(allowedDir)) {
        res.writeHead(403, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "Forbidden" }));
        return;
      }
      if (!fs.existsSync(screenshotPath)) {
        res.writeHead(404, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "Screenshot not found" }));
        return;
      }
      res.writeHead(200, { "Content-Type": "image/webp" });
      const stream = fs.createReadStream(screenshotPath);
      stream.on("error", () => {
        if (!res.headersSent) {
          res.writeHead(500, { "Content-Type": "application/json" });
        }
        res.end();
      });
      stream.pipe(res);
      return;
    }

    // --- ABP Control ---
    if (req.method === "GET" && pathname === "/control/status") {
      const running = await checkAbpStatus(args.abpUrl);
      // Auto-reattach on transition to running, or when running but DB is missing
      if (running && (!lastAbpRunning || !db)) {
        const sessionData = await fetchAbpSessionData(args.abpUrl);
        if (sessionData && sessionData.session_dir !== currentSessionDir) {
          openSessionDb(sessionData.session_dir);
          broadcastSSE(JSON.stringify({ type: "session_changed", session_dir: currentSessionDir }));
        } else if (!db && sessionData) {
          openSessionDb(sessionData.session_dir);
          broadcastSSE(JSON.stringify({ type: "session_changed", session_dir: currentSessionDir }));
        }
      }
      if (!running && lastAbpRunning) {
        // ABP went offline — close stale DB so next startup gets fresh state
        if (db) { try { db.close(); } catch { /* ignore */ } db = null; }
        if (watcher) { watcher.close(); watcher = null; }
        sessionId = "";
        currentSessionDir = "";
        broadcastSSE(JSON.stringify({ type: "session_changed", session_dir: "" }));
      }
      lastAbpRunning = running;
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({
        running,
        managed: abpProcess !== null,
        session_dir: currentSessionDir || null,
        launch_session_dir: args.sessionDir,
        abp_binary: args.abpBinary,
      }));
      return;
    }

    if (req.method === "POST" && pathname === "/control/attach") {
      const attached = await attachToAbp();
      if (!attached) {
        res.writeHead(400, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ ok: false, error: "Could not reach ABP or get session data" }));
        return;
      }
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({
        ok: true,
        session_dir: currentSessionDir,
      }));
      return;
    }

    if (req.method === "POST" && pathname === "/control/start") {
      const body = await readJsonBody(req);
      const sessionDir = (body.session_dir as string) || args.sessionDir;
      const config: LaunchConfig = {
        sessionDir: path.resolve(sessionDir),
        executablePath: (body.executable_path as string) || args.abpBinary,
        windowWidth: body.window_width as number | undefined,
        windowHeight: body.window_height as number | undefined,
        headless: body.headless as boolean | undefined,
        extraArgs: body.extra_args as string[] | undefined,
      };
      const result = await startAbp(args.abpUrl, config);
      if (result.ok) {
        // Auto-detect session dir from the newly launched ABP
        const tryAttach = async () => {
          for (let i = 0; i < 10; i++) {
            if (await attachToAbp()) return;
            await new Promise((r) => setTimeout(r, 500));
          }
        };
        tryAttach();
      }
      res.writeHead(result.ok ? 200 : 400, { "Content-Type": "application/json" });
      res.end(JSON.stringify(result));
      return;
    }

    if (req.method === "POST" && pathname === "/control/stop") {
      const result = await stopAbp(args.abpUrl);
      res.writeHead(result.ok ? 200 : 400, { "Content-Type": "application/json" });
      res.end(JSON.stringify(result));
      return;
    }

    if (req.method === "POST" && pathname === "/control/restart") {
      const body = await readJsonBody(req);
      await stopAbp(args.abpUrl);
      await new Promise((r) => setTimeout(r, 1000));
      const sessionDir = (body.session_dir as string) || args.sessionDir;
      const config: LaunchConfig = {
        sessionDir: path.resolve(sessionDir),
        executablePath: (body.executable_path as string) || args.abpBinary,
        windowWidth: body.window_width as number | undefined,
        windowHeight: body.window_height as number | undefined,
        headless: body.headless as boolean | undefined,
        extraArgs: body.extra_args as string[] | undefined,
      };
      const result = await startAbp(args.abpUrl, config);
      if (result.ok) {
        const tryAttach = async () => {
          for (let i = 0; i < 10; i++) {
            if (await attachToAbp()) return;
            await new Promise((r) => setTimeout(r, 500));
          }
        };
        tryAttach();
      }
      res.writeHead(result.ok ? 200 : 400, { "Content-Type": "application/json" });
      res.end(JSON.stringify(result));
      return;
    }

    // --- Proxy to ABP ---
    if (pathname.startsWith("/api/v1/")) {
      proxyToAbp(args.abpUrl, req, res);
      return;
    }

    // --- 404 ---
    res.writeHead(404, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ error: "Not found" }));
  });

  server.listen(args.port, () => {
    console.log(`ABP Debug Server`);
    console.log(`  UI:          http://localhost:${args.port}`);
    console.log(`  ABP:         ${args.abpUrl}`);
    console.log(`  Binary:      ${args.abpBinary || "(not found)"}`);
    if (currentSessionDir) {
      console.log(`  Session dir: ${currentSessionDir}`);
      if (sessionId) console.log(`  Session:     ${sessionId}`);
    } else {
      console.log(`  Session dir: (waiting for ABP connection)`);
    }
    console.log(`\nPress Ctrl+C to stop.\n`);
  });

  const shutdown = () => {
    console.log("\nShutting down...");
    if (watcher) watcher.close();
    if (db) db.close();
    server.close();
    process.exit(0);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

main();
