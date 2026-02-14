#!/usr/bin/env node

import { launch } from "../launch.js";
import { ABP_VERSION, CHROME_VERSION } from "../paths.js";

function parseArgs(argv: string[]): { port: number; chromeArgs: string[] } {
  let port = 8222;
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
    } else if (argv[i] === "--help" || argv[i] === "-h") {
      console.log(`agent-browser-protocol v${ABP_VERSION} (Chrome ${CHROME_VERSION})

Usage:
  agent-browser-protocol [options] [-- chrome-args...]

Options:
  --port <port>   Port to listen on (default: 8222)
  --help, -h      Show this help message

Examples:
  agent-browser-protocol
  agent-browser-protocol --port 9222
  agent-browser-protocol -- --disable-gpu --window-size=1920,1080`);
      process.exit(0);
    } else {
      console.error(`Unknown option: ${argv[i]}`);
      console.error('Run with --help for usage information');
      process.exit(1);
    }
  }

  return { port, chromeArgs };
}

async function main() {
  const { port, chromeArgs } = parseArgs(process.argv);

  console.log(`Agent Browser Protocol v${ABP_VERSION} (Chrome ${CHROME_VERSION})`);
  console.log(`Starting on port ${port}...`);

  const browser = await launch({ port, args: chromeArgs });

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
