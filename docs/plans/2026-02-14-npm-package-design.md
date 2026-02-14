# ABP npm Package Design

## Overview

Wrap ABP distribution into an npm package (`agent-browser-protocol`) so developers can install and use ABP with a single `npm install`. The package downloads pre-built platform binaries from GitHub Releases, provides a typed TypeScript client SDK mirroring the REST API, and manages the browser process lifecycle.

## Package Structure

```
tools/abp-npm/
├── package.json
├── tsconfig.json
├── src/
│   ├── index.ts              # Public API exports
│   ├── install.ts            # postinstall: download binary from GitHub Releases
│   ├── launch.ts             # launch(): spawn ABP process, wait for ready
│   └── client.ts             # ABPClient: typed REST API wrapper
├── bin/
│   └── abp.ts                # CLI entry point
└── browsers/                 # Downloaded binaries (gitignored)
    └── <platform-arch>/
```

## Binary Download (install.ts)

On `npm install`, the postinstall script:

1. Detects platform: `darwin-arm64`, `darwin-x64`, `linux-x64`, `win32-x64`
2. Constructs GitHub Release URL: `https://github.com/<owner>/<repo>/releases/download/v{version}/abp-{version}-chrome-{chromeVersion}-{platform}-{arch}.{zip|tar.gz}`
3. Downloads and extracts to `browsers/<platform>/`
4. Verifies binary exists and is executable

Environment variables:
- `ABP_SKIP_DOWNLOAD=1` — skip download (CI or pre-existing installs)
- `ABP_BROWSER_PATH` — use a custom binary path

Version mapping: npm package version = `ABP_VERSION`. Chrome version stored as constant in the package.

## Client SDK (client.ts)

Thin, typed 1:1 mapping to the REST API. Zero runtime dependencies — uses Node.js built-in `node:http`.

```typescript
const client = new ABPClient("http://localhost:8222");

// Tabs
const tabs = await client.tabs.list();
const tab = await client.tabs.create({ url: "https://example.com" });
await client.tabs.close(tabId);
await client.tabs.activate(tabId);
await client.tabs.stop(tabId);

// Navigation
await client.tabs.navigate(tabId, { url: "https://example.com" });
await client.tabs.reload(tabId);
await client.tabs.back(tabId);
await client.tabs.forward(tabId);

// Interaction
await client.tabs.click(tabId, { x: 100, y: 200 });
await client.tabs.type(tabId, { text: "hello" });
await client.tabs.scroll(tabId, { x: 500, y: 400, delta_y: 300 });
await client.tabs.keyPress(tabId, { key: "a", modifiers: ["Control"] });

// Content
const screenshot = await client.tabs.screenshot(tabId, { markup: "interactive" });
const result = await client.tabs.execute(tabId, { script: "document.title" });
const text = await client.tabs.text(tabId);

// Browser
const status = await client.browser.status();
await client.browser.shutdown();
```

Every method returns parsed JSON. Screenshot binary endpoint returns a `Buffer`.

## Launch (launch.ts)

```typescript
import { launch } from "agent-browser-protocol";

const browser = await launch({
  port: 8222,                // default
  headless: false,           // default
  sessionDir: "/tmp/abp",   // optional, auto-generated if omitted
  executablePath: "...",     // optional, override binary path
  args: ["--disable-gpu"],  // Chrome command-line args, passed through as-is
});

const tabs = await browser.client.tabs.list();
await browser.close(); // sends /browser/shutdown, waits for process exit
```

`launch()` spawns the ABP process with `--abp-port`, `--abp-session-dir`, plus any user-provided `args`. Polls `/api/v1/browser/status` until ready (10s timeout). Returns `{ client, process, close() }`.

## CLI (bin/abp.ts)

```bash
npx agent-browser-protocol
npx agent-browser-protocol --port 9222
npx agent-browser-protocol --port 9222 -- --disable-gpu --window-size=1920,1080
```

Everything after `--` is passed as Chrome args.

## Build & Publish

- **Bundler**: `tsup` — ESM + CJS dual output
- **Types**: TypeScript `.d.ts` generation
- **Output**: `dist/` within package directory

```json
{
  "name": "agent-browser-protocol",
  "exports": {
    ".": { "import": "./dist/index.mjs", "require": "./dist/index.cjs" }
  },
  "types": "./dist/index.d.ts",
  "bin": { "agent-browser-protocol": "./dist/bin/abp.mjs" },
  "scripts": {
    "build": "tsup",
    "postinstall": "node dist/install.mjs"
  },
  "files": ["dist/", "browsers/"]
}
```

**GitHub Release naming** (matches existing release scripts):
- `abp-{version}-chrome-{chromeVersion}-mac-arm64.zip`
- `abp-{version}-chrome-{chromeVersion}-mac-x64.zip`
- `abp-{version}-chrome-{chromeVersion}-linux-x64.tar.gz`
- `abp-{version}-chrome-{chromeVersion}-win-x64.zip`

**Publish flow:**
1. Build release binaries with existing `tools/abp/release-*.sh` scripts
2. Upload to GitHub Releases tagged `v{version}`
3. `cd tools/abp-npm && npm run build && npm publish`

## Platforms

- macOS arm64
- macOS x64
- Linux x64
- Windows x64
