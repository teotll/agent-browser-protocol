#!/usr/bin/env node

import { launch } from "../launch.js";
import { ABP_VERSION, CHROME_VERSION } from "../paths.js";

interface ParsedArgs {
  port: number;
  headless: boolean;
  sessionDir?: string;
  chromeArgs: string[];
}

function parseArgs(argv: string[]): ParsedArgs {
  let port = parseInt(process.env.ABP_PORT || "8222", 10);
  let headless = process.env.ABP_HEADLESS === "1";
  let sessionDir: string | undefined;
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
    } else if (argv[i] === "--session-dir" && i + 1 < argv.length) {
      sessionDir = argv[i + 1];
      i++;
    } else if (argv[i].startsWith("--session-dir=")) {
      sessionDir = argv[i].split("=").slice(1).join("=");
    } else if (argv[i] === "--help" || argv[i] === "-h") {
      console.log(`agent-browser-protocol v${ABP_VERSION} (Chrome ${CHROME_VERSION})

Usage:
  agent-browser-protocol [options] [-- chrome-args...]

Options:
  --port <port>          Port to listen on (default: 8222)
  --headless             Run without a visible window
  --session-dir <path>   Directory for session data (database, screenshots)
  --help, -h             Show this help message

Environment Variables:
  ABP_PORT               Port to listen on (overridden by --port)
  ABP_HEADLESS=1         Run headless (overridden by --headless)
  ABP_BROWSER_PATH       Path to a custom ABP binary
  ABP_SKIP_DOWNLOAD=1    Skip binary download during install

Examples:
  agent-browser-protocol
  agent-browser-protocol --port 9222
  agent-browser-protocol --headless
  agent-browser-protocol --session-dir ./my-session
  agent-browser-protocol -- --disable-gpu`);
      process.exit(0);
    } else {
      console.error(`Unknown option: ${argv[i]}`);
      console.error('Run with --help for usage information');
      process.exit(1);
    }
  }

  return { port, headless, sessionDir, chromeArgs };
}

async function main() {
  const { port, headless, sessionDir, chromeArgs } = parseArgs(process.argv);

  console.log(`Agent Browser Protocol v${ABP_VERSION} (Chrome ${CHROME_VERSION})`);
  console.log(`Starting on port ${port}...`);

  const browser = await launch({ port, headless, sessionDir, args: chromeArgs });

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
