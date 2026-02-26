#!/usr/bin/env node

import { launch } from "../launch.js";
import { ABP_VERSION } from "../paths.js";

interface ParsedArgs {
  port: number;
  headless: boolean;
  verbose: boolean;
  sessionDir?: string;
  minWait?: number;
  trackingTimeout?: number;
  postSettle?: number;
  userDataDir?: string;
  profileDirectory?: string;
  userAgent?: string;
  chromeArgs: string[];
}

function parseArgs(argv: string[]): ParsedArgs {
  let port = parseInt(process.env.ABP_PORT || "8222", 10);
  let headless = process.env.ABP_HEADLESS === "1";
  let verbose = process.env.ABP_VERBOSE === "1";
  let sessionDir: string | undefined;
  let minWait: number | undefined = process.env.ABP_MIN_WAIT
    ? parseInt(process.env.ABP_MIN_WAIT, 10)
    : undefined;
  let trackingTimeout: number | undefined = process.env.ABP_TRACKING_TIMEOUT
    ? parseInt(process.env.ABP_TRACKING_TIMEOUT, 10)
    : undefined;
  let postSettle: number | undefined = process.env.ABP_POST_SETTLE
    ? parseInt(process.env.ABP_POST_SETTLE, 10)
    : undefined;
  let userDataDir: string | undefined = process.env.ABP_USER_DATA_DIR || undefined;
  let profileDirectory: string | undefined = process.env.ABP_PROFILE_DIRECTORY || undefined;
  let userAgent: string | undefined = process.env.ABP_USER_AGENT || undefined;
  const chromeArgs: string[] = [];
  let pastSeparator = false;

  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === "--") {
      pastSeparator = true;
      continue;
    }

    if (pastSeparator) {
      chromeArgs.push(argv[i]);
      continue;
    }

    if (argv[i] === "--port" && i + 1 < argv.length) {
      port = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--port=")) {
      port = parseInt(argv[i].split("=")[1], 10);
    } else if (argv[i] === "--headless") {
      headless = true;
    } else if (argv[i] === "--verbose" || argv[i] === "-v") {
      verbose = true;
    } else if (argv[i] === "--session-dir" && i + 1 < argv.length) {
      sessionDir = argv[i + 1];
      i++;
    } else if (argv[i].startsWith("--session-dir=")) {
      sessionDir = argv[i].split("=").slice(1).join("=");
    } else if (argv[i] === "--min-wait" && i + 1 < argv.length) {
      minWait = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--min-wait=")) {
      minWait = parseInt(argv[i].split("=")[1], 10);
    } else if (argv[i] === "--tracking-timeout" && i + 1 < argv.length) {
      trackingTimeout = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--tracking-timeout=")) {
      trackingTimeout = parseInt(argv[i].split("=")[1], 10);
    } else if (argv[i] === "--post-settle" && i + 1 < argv.length) {
      postSettle = parseInt(argv[i + 1], 10);
      i++;
    } else if (argv[i].startsWith("--post-settle=")) {
      postSettle = parseInt(argv[i].split("=")[1], 10);
    } else if (argv[i] === "--user-data-dir" && i + 1 < argv.length) {
      userDataDir = argv[i + 1];
      i++;
    } else if (argv[i].startsWith("--user-data-dir=")) {
      userDataDir = argv[i].split("=").slice(1).join("=");
    } else if (argv[i] === "--profile-directory" && i + 1 < argv.length) {
      profileDirectory = argv[i + 1];
      i++;
    } else if (argv[i].startsWith("--profile-directory=")) {
      profileDirectory = argv[i].split("=").slice(1).join("=");
    } else if (argv[i] === "--user-agent" && i + 1 < argv.length) {
      userAgent = argv[i + 1];
      i++;
    } else if (argv[i].startsWith("--user-agent=")) {
      userAgent = argv[i].split("=").slice(1).join("=");
    } else if (argv[i] === "--help" || argv[i] === "-h") {
      console.log(`agent-browser-protocol v${ABP_VERSION}

Usage:
  agent-browser-protocol [options] [-- chrome-args...]

Options:
  --port <port>          Port to listen on (default: 8222)
  --headless             Run without a visible window
  --verbose, -v          Show browser output (pipe to stderr)
  --session-dir <path>   Directory for session data (database, screenshots)
  --min-wait <ms>        Pre-network settlement wait in ms (default: 250)
  --tracking-timeout <ms> Request tracking timeout in ms (default: 1000)
  --post-settle <ms>     Post-network settle time in ms (default: 750)
  --user-data-dir <path>   Chrome user data directory
  --profile-directory <name> Chrome profile directory name
  --user-agent <string>    Custom User-Agent string
  --mcp                  Run as MCP server (JSON-RPC over stdio)
  --help, -h             Show this help message

Environment Variables:
  ABP_PORT               Port to listen on (overridden by --port)
  ABP_HEADLESS=1         Run headless (overridden by --headless)
  ABP_VERBOSE=1          Show browser output (overridden by --verbose)
  ABP_MIN_WAIT           Pre-network settlement wait in ms
  ABP_TRACKING_TIMEOUT   Request tracking timeout in ms
  ABP_POST_SETTLE        Post-network settle time in ms
  ABP_USER_DATA_DIR        Chrome user data directory (overridden by --user-data-dir)
  ABP_PROFILE_DIRECTORY    Chrome profile directory name (overridden by --profile-directory)
  ABP_USER_AGENT           Custom User-Agent string (overridden by --user-agent)
  ABP_BROWSER_PATH       Path to a custom ABP binary
  ABP_SKIP_DOWNLOAD=1    Skip binary download during install

Examples:
  agent-browser-protocol
  agent-browser-protocol --port 9222
  agent-browser-protocol --headless
  agent-browser-protocol --session-dir ./my-session
  agent-browser-protocol -- --disable-gpu
  agent-browser-protocol --user-data-dir /tmp/profile --user-agent "MyBot/1.0"`);
      process.exit(0);
    } else {
      console.error(`Unknown option: ${argv[i]}`);
      console.error('Run with --help for usage information');
      process.exit(1);
    }
  }

  return { port, headless, verbose, sessionDir, minWait, trackingTimeout, postSettle, userDataDir, profileDirectory, userAgent, chromeArgs };
}

async function main() {
  if (process.argv.includes("--mcp")) {
    await import("../mcp-proxy.js");
    return;
  }

  const { port, headless, verbose, sessionDir, minWait, trackingTimeout, postSettle, userDataDir, profileDirectory, userAgent, chromeArgs } = parseArgs(process.argv);

  console.log(`Agent Browser Protocol v${ABP_VERSION}`);
  console.log(`Starting on port ${port}...`);

  const browser = await launch({ port, headless, verbose, sessionDir, minWait, trackingTimeout, postSettle, userDataDir, profileDirectory, userAgent, args: chromeArgs });

  console.log(`\nABP is ready!`);
  console.log(`  API:  http://localhost:${port}/api/v1`);
  console.log(`  MCP:  http://localhost:${port}/mcp`);
  console.log(`\nPress Ctrl+C to stop.\n`);

  const shutdown = async () => {
    console.log("\nShutting down...");
    await browser.close();
    process.exit(0);
  };

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);

  await new Promise(() => {});
}

main().catch((err) => {
  console.error("Error:", err.message);
  process.exit(1);
});
