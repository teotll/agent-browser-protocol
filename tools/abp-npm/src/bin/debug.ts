#!/usr/bin/env node

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import Database from "better-sqlite3";

// --- CLI Arg Parsing ---

interface DebugArgs {
  port: number;
  abpUrl: string;
  sessionDir: string;
}

function parseArgs(argv: string[]): DebugArgs {
  let port = 8223;
  let abpUrl = "http://localhost:8222";
  let sessionDir = "";

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
    } else if (arg === "--help" || arg === "-h") {
      console.log(`abp-debug — ABP Debug Server

Usage:
  abp-debug --session-dir <path> [options]

Options:
  --session-dir <path>   Path to ABP session directory (required)
  --port <port>          Debug server port (default: 8223)
  --abp-url <url>        ABP base URL (default: http://localhost:8222)
  --help, -h             Show this help message`);
      process.exit(0);
    } else {
      console.error(`Unknown option: ${arg}\nRun with --help for usage.`);
      process.exit(1);
    }
  }

  if (!sessionDir) {
    console.error("Error: --session-dir is required.\nRun with --help for usage.");
    process.exit(1);
  }

  return { port, abpUrl: abpUrl.replace(/\/+$/, ""), sessionDir: path.resolve(sessionDir) };
}

// --- SQLite ---

function openDatabase(sessionDir: string): Database.Database {
  const dbPath = path.join(sessionDir, "history.db");
  if (!fs.existsSync(dbPath)) {
    throw new Error(`Database not found: ${dbPath}`);
  }
  return new Database(dbPath, { readonly: true });
}

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

// --- HTML ---

function getHtmlPath(): string {
  try {
    const dir = path.dirname(fileURLToPath(import.meta.url));
    return path.resolve(dir, "..", "src", "debug-ui.html");
  } catch {
    return path.resolve(__dirname, "..", "src", "debug-ui.html");
  }
}

// --- Main ---

function main() {
  const args = parseArgs(process.argv);
  const db = openDatabase(args.sessionDir);
  const session = getSession(db);

  if (!session) {
    console.error("Error: No session found in database.");
    process.exit(1);
  }

  const sessionId = session.id as string;
  let lastMaxId = getMaxActionId(db, sessionId);

  // Load HTML
  const htmlPath = getHtmlPath();
  if (!fs.existsSync(htmlPath)) {
    console.error(`Error: UI file not found: ${htmlPath}`);
    process.exit(1);
  }
  const html = fs.readFileSync(htmlPath, "utf-8");

  // fs.watch for real-time updates
  let debounceTimer: ReturnType<typeof setTimeout> | null = null;
  const watcher = fs.watch(args.sessionDir, { recursive: true }, () => {
    if (debounceTimer) clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      try {
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

  // HTTP server
  const server = http.createServer((req, res) => {
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
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({
        session,
        session_dir: args.sessionDir,
        abp_url: args.abpUrl,
        action_count: getMaxActionId(db, sessionId),
      }));
      return;
    }

    if (req.method === "GET" && pathname === "/data/actions") {
      const actions = getActions(db, sessionId);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(actions));
      return;
    }

    const actionMatch = pathname.match(/^\/data\/actions\/(\d+)$/);
    if (req.method === "GET" && actionMatch) {
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
      const screenshotPath = path.join(args.sessionDir, "screenshots", filename);
      const allowedDir = path.join(args.sessionDir, "screenshots") + path.sep;
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
    console.log(`  Session dir: ${args.sessionDir}`);
    console.log(`  Session:     ${sessionId}`);
    console.log(`\nPress Ctrl+C to stop.\n`);
  });

  const shutdown = () => {
    console.log("\nShutting down...");
    watcher.close();
    db.close();
    server.close();
    process.exit(0);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

main();
