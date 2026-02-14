# `agent-browser-protocol` npm Package Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Create an npm package that downloads pre-built ABP binaries from GitHub Releases, provides a typed TypeScript client SDK mirroring the REST API, and manages the browser process lifecycle.

**Architecture:** A single npm package (`agent-browser-protocol`) in `tools/abp-npm/` with four modules: install (postinstall binary download), client (typed REST wrapper), launch (process lifecycle), and CLI (npx entry point). Zero runtime dependencies — uses only Node.js built-ins.

**Tech Stack:** TypeScript, tsup (bundler), Node.js built-ins (`node:http`, `node:child_process`, `node:fs`, `node:path`)

---

### Task 1: Project Scaffold

**Files:**
- Create: `tools/abp-npm/package.json`
- Create: `tools/abp-npm/tsconfig.json`
- Create: `tools/abp-npm/tsup.config.ts`
- Create: `tools/abp-npm/.gitignore`
- Create: `tools/abp-npm/src/index.ts`

**Step 1: Create package.json**

```json
{
  "name": "agent-browser-protocol",
  "version": "0.1.0",
  "description": "Agent Browser Protocol - AI agent browser control at the engine level",
  "type": "module",
  "exports": {
    ".": {
      "import": "./dist/index.mjs",
      "require": "./dist/index.cjs",
      "types": "./dist/index.d.ts"
    }
  },
  "main": "./dist/index.cjs",
  "module": "./dist/index.mjs",
  "types": "./dist/index.d.ts",
  "bin": {
    "agent-browser-protocol": "./dist/bin/abp.mjs"
  },
  "files": [
    "dist/",
    "browsers/"
  ],
  "scripts": {
    "build": "tsup",
    "postinstall": "node dist/install.mjs",
    "typecheck": "tsc --noEmit"
  },
  "keywords": ["browser", "automation", "ai", "agent", "chromium", "cdp"],
  "license": "MIT",
  "engines": {
    "node": ">=18"
  },
  "devDependencies": {
    "tsup": "^8.0.0",
    "typescript": "^5.4.0"
  }
}
```

**Step 2: Create tsconfig.json**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "ESNext",
    "moduleResolution": "bundler",
    "strict": true,
    "esModuleInterop": true,
    "skipLibCheck": true,
    "outDir": "dist",
    "rootDir": "src",
    "declaration": true,
    "declarationMap": true,
    "sourceMap": true
  },
  "include": ["src/**/*.ts"]
}
```

**Step 3: Create tsup.config.ts**

```typescript
import { defineConfig } from "tsup";

export default defineConfig({
  entry: {
    index: "src/index.ts",
    install: "src/install.ts",
    "bin/abp": "src/bin/abp.ts",
  },
  format: ["esm", "cjs"],
  dts: { entry: "src/index.ts" },
  clean: true,
  splitting: false,
  sourcemap: true,
});
```

**Step 4: Create .gitignore**

```
node_modules/
dist/
browsers/
```

**Step 5: Create src/index.ts (placeholder exports)**

```typescript
export { ABPClient } from "./client.js";
export { launch } from "./launch.js";
export type { LaunchOptions, Browser } from "./launch.js";
```

**Step 6: Install dependencies**

Run: `cd tools/abp-npm && npm install`
Expected: node_modules created, lockfile generated

**Step 7: Commit**

```bash
git add tools/abp-npm/package.json tools/abp-npm/tsconfig.json tools/abp-npm/tsup.config.ts tools/abp-npm/.gitignore tools/abp-npm/src/index.ts tools/abp-npm/package-lock.json
git commit -m "feat: scaffold agent-browser-protocol npm package"
```

---

### Task 2: HTTP Helper

**Files:**
- Create: `tools/abp-npm/src/http.ts`

A small internal module wrapping `node:http` for typed JSON requests. Used by both the client SDK and the install script.

**Step 1: Create http.ts**

```typescript
import http from "node:http";
import https from "node:https";
import { URL } from "node:url";

export interface RequestOptions {
  method?: string;
  headers?: Record<string, string>;
  body?: unknown;
  timeout?: number;
}

export interface Response<T = unknown> {
  status: number;
  headers: Record<string, string | string[] | undefined>;
  data: T;
}

export async function request<T = unknown>(
  url: string,
  options: RequestOptions = {},
): Promise<Response<T>> {
  const { method = "GET", headers = {}, body, timeout = 30000 } = options;
  const parsed = new URL(url);
  const transport = parsed.protocol === "https:" ? https : http;

  return new Promise((resolve, reject) => {
    const req = transport.request(
      parsed,
      {
        method,
        headers: {
          ...(body !== undefined
            ? { "Content-Type": "application/json" }
            : {}),
          ...headers,
        },
        timeout,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk: Buffer) => chunks.push(chunk));
        res.on("end", () => {
          const raw = Buffer.concat(chunks);
          const contentType = res.headers["content-type"] || "";
          let data: unknown;
          if (contentType.includes("application/json")) {
            data = JSON.parse(raw.toString("utf-8"));
          } else if (
            contentType.includes("image/") ||
            contentType.includes("application/octet-stream")
          ) {
            data = raw;
          } else {
            data = raw.toString("utf-8");
          }
          resolve({
            status: res.statusCode || 0,
            headers: res.headers as Record<
              string,
              string | string[] | undefined
            >,
            data: data as T,
          });
        });
      },
    );
    req.on("error", reject);
    req.on("timeout", () => {
      req.destroy();
      reject(new Error(`Request to ${url} timed out after ${timeout}ms`));
    });
    if (body !== undefined) {
      req.write(JSON.stringify(body));
    }
    req.end();
  });
}

export async function downloadToFile(
  url: string,
  destPath: string,
  onRedirect?: (newUrl: string) => Promise<void>,
): Promise<void> {
  const fs = await import("node:fs");
  const parsed = new URL(url);
  const transport = parsed.protocol === "https:" ? https : http;

  return new Promise((resolve, reject) => {
    transport.get(parsed, { timeout: 300000 }, (res) => {
      // Handle redirects (GitHub Releases uses 302)
      if (
        (res.statusCode === 301 || res.statusCode === 302) &&
        res.headers.location
      ) {
        res.resume(); // Consume response to free socket
        downloadToFile(res.headers.location, destPath).then(resolve, reject);
        return;
      }

      if (res.statusCode !== 200) {
        res.resume();
        reject(
          new Error(
            `Download failed: HTTP ${res.statusCode} from ${url}`,
          ),
        );
        return;
      }

      const file = fs.createWriteStream(destPath);
      res.pipe(file);
      file.on("finish", () => {
        file.close();
        resolve();
      });
      file.on("error", (err) => {
        fs.unlinkSync(destPath);
        reject(err);
      });
    }).on("error", reject);
  });
}
```

**Step 2: Commit**

```bash
git add tools/abp-npm/src/http.ts
git commit -m "feat: add HTTP helper module for client and installer"
```

---

### Task 3: Install Script (Binary Download)

**Files:**
- Create: `tools/abp-npm/src/install.ts`
- Create: `tools/abp-npm/src/paths.ts`

**Step 1: Create paths.ts (platform detection and binary path resolution)**

```typescript
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = path.resolve(__dirname, "..");

export const ABP_VERSION = "0.1.0";
export const CHROME_VERSION = "146.0.7635.0";
export const GITHUB_REPO = "anthropics/anthropic-browser";

export interface PlatformInfo {
  platform: string; // "mac", "linux", "win"
  arch: string; // "arm64", "x64"
  archiveExt: string; // ".zip" or ".tar.gz"
  executablePath: string; // Relative path within extracted archive
}

export function getPlatformInfo(): PlatformInfo {
  const platform = process.platform;
  const arch = process.arch;

  switch (platform) {
    case "darwin":
      return {
        platform: "mac",
        arch: arch === "arm64" ? "arm64" : "x64",
        archiveExt: ".zip",
        executablePath: "ABP.app/Contents/MacOS/ABP",
      };
    case "linux":
      return {
        platform: "linux",
        arch: "x64",
        archiveExt: ".tar.gz",
        executablePath: "abp-chrome/abp",
      };
    case "win32":
      return {
        platform: "win",
        arch: "x64",
        archiveExt: ".zip",
        executablePath: "abp-chrome/chrome.exe",
      };
    default:
      throw new Error(`Unsupported platform: ${platform}`);
  }
}

export function getArchiveName(info: PlatformInfo): string {
  return `abp-${ABP_VERSION}-chrome-${CHROME_VERSION}-${info.platform}-${info.arch}${info.archiveExt}`;
}

export function getDownloadUrl(info: PlatformInfo): string {
  const archive = getArchiveName(info);
  return `https://github.com/${GITHUB_REPO}/releases/download/v${ABP_VERSION}/${archive}`;
}

export function getBrowsersDir(): string {
  return path.join(PACKAGE_ROOT, "browsers");
}

export function getExecutablePath(customPath?: string): string {
  if (customPath) return customPath;
  if (process.env.ABP_BROWSER_PATH) return process.env.ABP_BROWSER_PATH;

  const info = getPlatformInfo();
  return path.join(getBrowsersDir(), info.executablePath);
}
```

**Step 2: Create install.ts**

```typescript
import fs from "node:fs";
import path from "node:path";
import { execSync } from "node:child_process";
import {
  getPlatformInfo,
  getDownloadUrl,
  getArchiveName,
  getBrowsersDir,
  getExecutablePath,
} from "./paths.js";
import { downloadToFile } from "./http.js";

async function install() {
  // Skip if ABP_SKIP_DOWNLOAD is set
  if (process.env.ABP_SKIP_DOWNLOAD === "1") {
    console.log("ABP_SKIP_DOWNLOAD=1, skipping binary download");
    return;
  }

  // Skip if custom path is set and exists
  if (process.env.ABP_BROWSER_PATH) {
    if (fs.existsSync(process.env.ABP_BROWSER_PATH)) {
      console.log(
        `Using custom ABP binary: ${process.env.ABP_BROWSER_PATH}`,
      );
      return;
    }
    console.warn(
      `WARNING: ABP_BROWSER_PATH set but file not found: ${process.env.ABP_BROWSER_PATH}`,
    );
  }

  // Check if already downloaded
  const execPath = getExecutablePath();
  if (fs.existsSync(execPath)) {
    console.log(`ABP binary already exists: ${execPath}`);
    return;
  }

  const info = getPlatformInfo();
  const url = getDownloadUrl(info);
  const browsersDir = getBrowsersDir();
  const archiveName = getArchiveName(info);
  const archivePath = path.join(browsersDir, archiveName);

  console.log(`Downloading ABP for ${info.platform}-${info.arch}...`);
  console.log(`URL: ${url}`);

  // Create browsers directory
  fs.mkdirSync(browsersDir, { recursive: true });

  // Download archive
  await downloadToFile(url, archivePath);
  console.log(`Downloaded: ${archivePath}`);

  // Extract archive
  console.log("Extracting...");
  if (info.archiveExt === ".tar.gz") {
    execSync(`tar -xzf "${archivePath}" -C "${browsersDir}"`, {
      stdio: "inherit",
    });
  } else {
    // .zip
    if (process.platform === "win32") {
      execSync(
        `powershell -Command "Expand-Archive -Path '${archivePath}' -DestinationPath '${browsersDir}' -Force"`,
        { stdio: "inherit" },
      );
    } else {
      execSync(`unzip -qo "${archivePath}" -d "${browsersDir}"`, {
        stdio: "inherit",
      });
    }
  }

  // Remove archive
  fs.unlinkSync(archivePath);

  // Verify executable exists
  if (!fs.existsSync(execPath)) {
    throw new Error(`Extraction failed: executable not found at ${execPath}`);
  }

  // Make executable on unix
  if (process.platform !== "win32") {
    fs.chmodSync(execPath, 0o755);
  }

  console.log(`ABP installed: ${execPath}`);
}

install().catch((err) => {
  console.error("Failed to install ABP binary:", err.message);
  console.error(
    "You can set ABP_BROWSER_PATH to point to an existing ABP binary",
  );
  process.exit(1);
});
```

**Step 3: Commit**

```bash
git add tools/abp-npm/src/paths.ts tools/abp-npm/src/install.ts
git commit -m "feat: add binary download installer for postinstall"
```

---

### Task 4: Client SDK — Types

**Files:**
- Create: `tools/abp-npm/src/types.ts`

Define all request/response types based on the REST API spec (`plans/API.md`).

**Step 1: Create types.ts**

```typescript
// === Common Envelope Types ===

export interface WaitUntil {
  type?: "immediate" | "action_complete" | "time";
  timeout_ms?: number;
  duration_ms?: number;
}

export interface ScreenshotOptions {
  area?: "none" | "viewport";
  markup?: "none" | "interactive" | "clickable" | "typeable" | "inputs";
  cursor?: boolean;
  format?: string;
}

export interface ActionRequest {
  wait_until?: WaitUntil;
  screenshot?: ScreenshotOptions;
}

export interface ScreenshotData {
  data: string;
  width: number;
  height: number;
  virtual_time_ms: number;
  format: string;
}

export interface ScrollPosition {
  horizontal_percent: number;
  vertical_percent: number;
  horizontal_px: number;
  vertical_px: number;
  page_width: number;
  page_height: number;
  viewport_width: number;
  viewport_height: number;
}

export interface ActionTiming {
  action_started_ms: number;
  action_completed_ms: number;
  wait_completed_ms: number;
  duration_ms: number;
}

export interface ActionEvent {
  type: string;
  virtual_time_ms: number;
  data: Record<string, unknown>;
}

export interface ActionResponse<T = Record<string, unknown>> {
  result: T;
  screenshot_before?: ScreenshotData;
  screenshot_after?: ScreenshotData;
  scroll?: ScrollPosition;
  events?: ActionEvent[];
  timing?: ActionTiming;
}

export interface ErrorResponse {
  error: string;
}

// === Browser ===

export interface BrowserStatus {
  success: boolean;
  data: {
    ready: boolean;
    state: string;
    components: {
      http_server: boolean;
      browser_window: boolean;
      devtools: boolean;
    };
    message?: string;
  };
}

export interface SessionData {
  success: boolean;
  data: {
    session_dir: string;
    database_path: string;
    screenshots_dir: string;
    screenshots_enabled: boolean;
  };
}

// === Tabs ===

export interface Tab {
  id: string;
  url: string;
  title: string;
  active?: boolean;
  loading?: boolean;
}

export interface CreateTabOptions {
  url?: string;
  active?: boolean;
  index?: number;
}

export interface CreatedTab {
  id: string;
  url: string;
}

export interface ActivateResult {
  status: string;
  tab_id: string;
  index: number;
}

// === Navigation ===

export type Modifier = "Shift" | "Control" | "Alt" | "Meta"
  | "ShiftLeft" | "ShiftRight" | "ControlLeft" | "ControlRight"
  | "AltLeft" | "AltRight" | "MetaLeft" | "MetaRight";

export interface NavigateOptions extends ActionRequest {
  url: string;
  referrer?: string;
}

// === Mouse ===

export interface ClickOptions extends ActionRequest {
  x: number;
  y: number;
  button?: "left" | "right" | "middle";
  click_count?: number;
  modifiers?: Modifier[];
}

export interface MoveOptions {
  x: number;
  y: number;
}

export interface ScrollOptions extends ActionRequest {
  x: number;
  y: number;
  delta_x?: number;
  delta_y?: number;
}

// === Keyboard ===

export interface TypeOptions extends ActionRequest {
  text: string;
}

export interface KeyOptions extends ActionRequest {
  key: string;
  modifiers?: Modifier[];
}

// === Content ===

export interface ExecuteOptions extends ActionRequest {
  script: string;
}

export interface ExecuteResult {
  value: unknown;
  type: string;
}

export interface TextOptions extends ActionRequest {
  selector?: string;
}

export interface TextResult {
  text: string | null;
}

// === Wait ===

export interface WaitOptions extends ActionRequest {
  ms: number;
}

// === Dialogs ===

export interface DialogInfo {
  present: boolean;
  dialog_type?: string;
  message?: string;
  default_prompt?: string;
}

export interface AcceptDialogOptions {
  prompt_text?: string;
}

// === Execution Control ===

export interface ExecutionState {
  enabled: boolean;
  paused: boolean;
  virtual_time_base_ms?: number;
}

export interface SetExecutionOptions {
  paused: boolean;
  initial_virtual_time?: number;
}

// === Downloads ===

export interface Download {
  id: string;
  url: string;
  filename: string;
  path?: string;
  state: string;
  bytes_received: number;
  total_bytes: number;
  percent_complete?: number;
  mime_type: string;
  start_time: number;
  end_time?: number;
}

export interface ListDownloadsOptions {
  state?: string;
  limit?: number;
}

// === File Chooser ===

export interface FileChooserOpenOptions {
  files: string[];
}

export interface FileChooserSaveOptions {
  path: string;
}

export interface FileChooserCancelOptions {
  cancel: true;
}

export type FileChooserOptions =
  | FileChooserOpenOptions
  | FileChooserSaveOptions
  | FileChooserCancelOptions;

// === History ===

export interface Session {
  id: string;
  [key: string]: unknown;
}

export interface HistoryAction {
  id: string;
  [key: string]: unknown;
}

export interface HistoryEvent {
  id: string;
  [key: string]: unknown;
}

// === Shutdown ===

export interface ShutdownOptions {
  timeout_ms?: number;
}
```

**Step 2: Commit**

```bash
git add tools/abp-npm/src/types.ts
git commit -m "feat: add TypeScript types for ABP REST API"
```

---

### Task 5: Client SDK — Implementation

**Files:**
- Create: `tools/abp-npm/src/client.ts`

**Step 1: Create client.ts**

```typescript
import { request } from "./http.js";
import type {
  ActionResponse,
  BrowserStatus,
  SessionData,
  ShutdownOptions,
  Tab,
  CreateTabOptions,
  CreatedTab,
  ActivateResult,
  NavigateOptions,
  ClickOptions,
  MoveOptions,
  ScrollOptions,
  TypeOptions,
  KeyOptions,
  ScreenshotOptions,
  ExecuteOptions,
  ExecuteResult,
  TextOptions,
  TextResult,
  WaitOptions,
  DialogInfo,
  AcceptDialogOptions,
  ExecutionState,
  SetExecutionOptions,
  Download,
  ListDownloadsOptions,
  FileChooserOptions,
  Session,
  HistoryAction,
  HistoryEvent,
  ActionRequest,
} from "./types.js";

class BrowserAPI {
  constructor(private baseUrl: string) {}

  async status(): Promise<BrowserStatus> {
    const res = await request<BrowserStatus>(`${this.baseUrl}/browser/status`);
    return res.data;
  }

  async sessionData(): Promise<SessionData> {
    const res = await request<SessionData>(
      `${this.baseUrl}/browser/session-data`,
    );
    return res.data;
  }

  async shutdown(options?: ShutdownOptions): Promise<void> {
    await request(`${this.baseUrl}/browser/shutdown`, {
      method: "POST",
      body: options || {},
    });
  }
}

class TabsAPI {
  constructor(private baseUrl: string) {}

  async list(): Promise<Tab[]> {
    const res = await request<Tab[]>(`${this.baseUrl}/tabs`);
    return res.data;
  }

  async get(tabId: string): Promise<Tab> {
    const res = await request<Tab>(`${this.baseUrl}/tabs/${tabId}`);
    return res.data;
  }

  async create(options?: CreateTabOptions): Promise<CreatedTab> {
    const res = await request<CreatedTab>(`${this.baseUrl}/tabs`, {
      method: "POST",
      body: options || {},
    });
    return res.data;
  }

  async close(tabId: string): Promise<void> {
    await request(`${this.baseUrl}/tabs/${tabId}`, { method: "DELETE" });
  }

  async activate(tabId: string): Promise<ActivateResult> {
    const res = await request<ActivateResult>(
      `${this.baseUrl}/tabs/${tabId}/activate`,
      { method: "POST", body: {} },
    );
    return res.data;
  }

  async stop(tabId: string): Promise<{ status: string; tab_id: string }> {
    const res = await request<{ status: string; tab_id: string }>(
      `${this.baseUrl}/tabs/${tabId}/stop`,
      { method: "POST", body: {} },
    );
    return res.data;
  }

  // Navigation
  async navigate(
    tabId: string,
    options: NavigateOptions,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/navigate`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async reload(
    tabId: string,
    options?: ActionRequest,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/reload`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async back(tabId: string, options?: ActionRequest): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/back`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async forward(
    tabId: string,
    options?: ActionRequest,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/forward`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  // Mouse
  async click(
    tabId: string,
    options: ClickOptions,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/click`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async move(tabId: string, options: MoveOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/move`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async scroll(
    tabId: string,
    options: ScrollOptions,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/scroll`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  // Keyboard
  async type(tabId: string, options: TypeOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/type`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async keyPress(
    tabId: string,
    options: KeyOptions,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/keyboard/press`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async keyDown(tabId: string, options: KeyOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/keyboard/down`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async keyUp(tabId: string, options: KeyOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/keyboard/up`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  // Screenshots
  async screenshot(
    tabId: string,
    options?: ScreenshotOptions,
  ): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/screenshot`,
      { method: "POST", body: { screenshot: options || {} } },
    );
    return res.data;
  }

  async screenshotBinary(
    tabId: string,
    options?: { markup?: string },
  ): Promise<Buffer> {
    const query = options?.markup ? `?markup=${options.markup}` : "";
    const res = await request<Buffer>(
      `${this.baseUrl}/tabs/${tabId}/screenshot${query}`,
    );
    return res.data;
  }

  // Content
  async execute(
    tabId: string,
    options: ExecuteOptions,
  ): Promise<ActionResponse<ExecuteResult>> {
    const res = await request<ActionResponse<ExecuteResult>>(
      `${this.baseUrl}/tabs/${tabId}/execute`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  async text(
    tabId: string,
    options?: TextOptions,
  ): Promise<TextResult> {
    const res = await request<TextResult>(
      `${this.baseUrl}/tabs/${tabId}/text`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  // Wait
  async wait(tabId: string, options: WaitOptions): Promise<ActionResponse> {
    const res = await request<ActionResponse>(
      `${this.baseUrl}/tabs/${tabId}/wait`,
      { method: "POST", body: options },
    );
    return res.data;
  }

  // Dialogs
  async dialog(tabId: string): Promise<DialogInfo> {
    const res = await request<DialogInfo>(
      `${this.baseUrl}/tabs/${tabId}/dialog`,
    );
    return res.data;
  }

  async dialogAccept(
    tabId: string,
    options?: AcceptDialogOptions,
  ): Promise<{ success: boolean }> {
    const res = await request<{ success: boolean }>(
      `${this.baseUrl}/tabs/${tabId}/dialog/accept`,
      { method: "POST", body: options || {} },
    );
    return res.data;
  }

  async dialogDismiss(tabId: string): Promise<{ success: boolean }> {
    const res = await request<{ success: boolean }>(
      `${this.baseUrl}/tabs/${tabId}/dialog/dismiss`,
      { method: "POST", body: {} },
    );
    return res.data;
  }

  // Execution Control
  async execution(tabId: string): Promise<ExecutionState> {
    const res = await request<ExecutionState>(
      `${this.baseUrl}/tabs/${tabId}/execution`,
    );
    return res.data;
  }

  async setExecution(
    tabId: string,
    options: SetExecutionOptions,
  ): Promise<ExecutionState> {
    const res = await request<ExecutionState>(
      `${this.baseUrl}/tabs/${tabId}/execution`,
      { method: "POST", body: options },
    );
    return res.data;
  }
}

class DownloadsAPI {
  constructor(private baseUrl: string) {}

  async list(options?: ListDownloadsOptions): Promise<{ downloads: Download[] }> {
    const params = new URLSearchParams();
    if (options?.state) params.set("state", options.state);
    if (options?.limit) params.set("limit", String(options.limit));
    const query = params.toString() ? `?${params.toString()}` : "";
    const res = await request<{ downloads: Download[] }>(
      `${this.baseUrl}/downloads${query}`,
    );
    return res.data;
  }

  async get(downloadId: string): Promise<Download> {
    const res = await request<Download>(
      `${this.baseUrl}/downloads/${downloadId}`,
    );
    return res.data;
  }

  async cancel(downloadId: string): Promise<{ success: boolean; message: string }> {
    const res = await request<{ success: boolean; message: string }>(
      `${this.baseUrl}/downloads/${downloadId}/cancel`,
      { method: "POST", body: {} },
    );
    return res.data;
  }
}

class FileChooserAPI {
  constructor(private baseUrl: string) {}

  async provide(
    chooserId: string,
    options: FileChooserOptions,
  ): Promise<{ success: boolean; cancelled?: boolean }> {
    const res = await request<{ success: boolean; cancelled?: boolean }>(
      `${this.baseUrl}/file-chooser/${chooserId}`,
      { method: "POST", body: options },
    );
    return res.data;
  }
}

class HistoryAPI {
  constructor(private baseUrl: string) {}

  async sessions(): Promise<Session[]> {
    const res = await request<Session[]>(
      `${this.baseUrl}/history/sessions`,
    );
    return res.data;
  }

  async currentSession(): Promise<Session> {
    const res = await request<Session>(
      `${this.baseUrl}/history/sessions/current`,
    );
    return res.data;
  }

  async session(sessionId: string): Promise<Session> {
    const res = await request<Session>(
      `${this.baseUrl}/history/sessions/${sessionId}`,
    );
    return res.data;
  }

  async exportSession(sessionId: string): Promise<unknown> {
    const res = await request(`${this.baseUrl}/history/sessions/${sessionId}/export`);
    return res.data;
  }

  async actions(): Promise<HistoryAction[]> {
    const res = await request<HistoryAction[]>(
      `${this.baseUrl}/history/actions`,
    );
    return res.data;
  }

  async action(actionId: string): Promise<HistoryAction> {
    const res = await request<HistoryAction>(
      `${this.baseUrl}/history/actions/${actionId}`,
    );
    return res.data;
  }

  async actionScreenshot(actionId: string): Promise<Buffer> {
    const res = await request<Buffer>(
      `${this.baseUrl}/history/actions/${actionId}/screenshot`,
    );
    return res.data;
  }

  async deleteActions(): Promise<void> {
    await request(`${this.baseUrl}/history/actions`, { method: "DELETE" });
  }

  async events(): Promise<HistoryEvent[]> {
    const res = await request<HistoryEvent[]>(
      `${this.baseUrl}/history/events`,
    );
    return res.data;
  }

  async event(eventId: string): Promise<HistoryEvent> {
    const res = await request<HistoryEvent>(
      `${this.baseUrl}/history/events/${eventId}`,
    );
    return res.data;
  }

  async deleteEvents(): Promise<void> {
    await request(`${this.baseUrl}/history/events`, { method: "DELETE" });
  }

  async deleteAll(): Promise<void> {
    await request(`${this.baseUrl}/history`, { method: "DELETE" });
  }
}

export class ABPClient {
  readonly browser: BrowserAPI;
  readonly tabs: TabsAPI;
  readonly downloads: DownloadsAPI;
  readonly fileChooser: FileChooserAPI;
  readonly history: HistoryAPI;

  constructor(baseUrl: string = "http://localhost:8222/api/v1") {
    // Strip trailing slash
    const url = baseUrl.replace(/\/+$/, "");
    this.browser = new BrowserAPI(url);
    this.tabs = new TabsAPI(url);
    this.downloads = new DownloadsAPI(url);
    this.fileChooser = new FileChooserAPI(url);
    this.history = new HistoryAPI(url);
  }
}
```

**Step 2: Commit**

```bash
git add tools/abp-npm/src/client.ts
git commit -m "feat: add ABPClient SDK with typed REST API wrappers"
```

---

### Task 6: Launch Module

**Files:**
- Create: `tools/abp-npm/src/launch.ts`

**Step 1: Create launch.ts**

```typescript
import { spawn, type ChildProcess } from "node:child_process";
import { ABPClient } from "./client.js";
import { getExecutablePath } from "./paths.js";
import { request } from "./http.js";

export interface LaunchOptions {
  port?: number;
  sessionDir?: string;
  executablePath?: string;
  headless?: boolean;
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
    args = [],
  } = options;

  const binaryPath = getExecutablePath(executablePath);

  const launchArgs: string[] = [
    `--abp-port=${port}`,
    "--no-first-run",
    "--no-default-browser-check",
  ];

  if (sessionDir) {
    launchArgs.push(`--abp-session-dir=${sessionDir}`);
  }

  if (headless) {
    launchArgs.push("--headless=new");
  }

  // Append user-provided Chrome args
  launchArgs.push(...args);

  const child = spawn(binaryPath, launchArgs, {
    stdio: "ignore",
    detached: false,
  });

  // Handle spawn errors
  const spawnError = new Promise<never>((_, reject) => {
    child.on("error", (err) => {
      reject(new Error(`Failed to launch ABP: ${err.message}`));
    });
  });

  const baseUrl = `http://localhost:${port}/api/v1`;

  // Race between ready and spawn error
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
    // Wait for process exit
    await new Promise<void>((resolve) => {
      if (child.exitCode !== null) {
        resolve();
        return;
      }
      child.on("exit", () => resolve());
      // Force kill after 5s
      setTimeout(() => {
        child.kill("SIGKILL");
        resolve();
      }, 5000);
    });
  };

  return { client, process: child, port, close };
}
```

**Step 2: Commit**

```bash
git add tools/abp-npm/src/launch.ts
git commit -m "feat: add launch() for browser process lifecycle management"
```

---

### Task 7: CLI Entry Point

**Files:**
- Create: `tools/abp-npm/src/bin/abp.ts`

**Step 1: Create bin/abp.ts**

```typescript
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

  // Handle graceful shutdown
  const shutdown = async () => {
    console.log("\nShutting down...");
    await browser.close();
    process.exit(0);
  };

  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);

  // Keep process alive
  await new Promise(() => {});
}

main().catch((err) => {
  console.error("Error:", err.message);
  process.exit(1);
});
```

**Step 2: Commit**

```bash
git add tools/abp-npm/src/bin/abp.ts
git commit -m "feat: add CLI entry point for npx agent-browser-protocol"
```

---

### Task 8: Wire Up Exports and Build

**Files:**
- Modify: `tools/abp-npm/src/index.ts`

**Step 1: Update src/index.ts with all exports**

```typescript
export { ABPClient } from "./client.js";
export { launch } from "./launch.js";
export { getExecutablePath } from "./paths.js";
export { ABP_VERSION, CHROME_VERSION } from "./paths.js";
export type { LaunchOptions, Browser } from "./launch.js";
export type {
  // Common
  WaitUntil,
  ScreenshotOptions,
  ActionRequest,
  ActionResponse,
  ScreenshotData,
  ScrollPosition,
  ActionTiming,
  ActionEvent,
  ErrorResponse,
  // Browser
  BrowserStatus,
  SessionData,
  ShutdownOptions,
  // Tabs
  Tab,
  CreateTabOptions,
  CreatedTab,
  ActivateResult,
  // Navigation
  Modifier,
  NavigateOptions,
  // Mouse
  ClickOptions,
  MoveOptions,
  ScrollOptions,
  // Keyboard
  TypeOptions,
  KeyOptions,
  // Content
  ExecuteOptions,
  ExecuteResult,
  TextOptions,
  TextResult,
  // Wait
  WaitOptions,
  // Dialogs
  DialogInfo,
  AcceptDialogOptions,
  // Execution Control
  ExecutionState,
  SetExecutionOptions,
  // Downloads
  Download,
  ListDownloadsOptions,
  // File Chooser
  FileChooserOptions,
  FileChooserOpenOptions,
  FileChooserSaveOptions,
  FileChooserCancelOptions,
  // History
  Session,
  HistoryAction,
  HistoryEvent,
} from "./types.js";
```

**Step 2: Run build**

Run: `cd tools/abp-npm && npx tsup`
Expected: Build succeeds, `dist/` directory created with `index.mjs`, `index.cjs`, `index.d.ts`, `install.mjs`, `bin/abp.mjs`

**Step 3: Run typecheck**

Run: `cd tools/abp-npm && npx tsc --noEmit`
Expected: No type errors

**Step 4: Commit**

```bash
git add tools/abp-npm/src/index.ts
git commit -m "feat: wire up all exports and verify build"
```

---

### Task 9: README

**Files:**
- Create: `tools/abp-npm/README.md`

**Step 1: Create README.md**

```markdown
# agent-browser-protocol

AI agent browser control at the engine level. A Chromium fork with a REST API for browser automation.

## Install

```bash
npm install agent-browser-protocol
```

This downloads the pre-built ABP browser binary for your platform (~130MB).

## Quick Start

```typescript
import { launch } from "agent-browser-protocol";

const browser = await launch();

// Navigate
const tabs = await browser.client.tabs.list();
const tabId = tabs[0].id;
await browser.client.tabs.navigate(tabId, { url: "https://example.com" });

// Screenshot
const screenshot = await browser.client.tabs.screenshotBinary(tabId, {
  markup: "interactive",
});
fs.writeFileSync("screenshot.webp", screenshot);

// Interact
await browser.client.tabs.click(tabId, { x: 100, y: 200 });
await browser.client.tabs.type(tabId, { text: "hello world" });

// Cleanup
await browser.close();
```

## CLI

```bash
# Launch ABP
npx agent-browser-protocol

# Custom port
npx agent-browser-protocol --port 9222

# Pass Chrome flags
npx agent-browser-protocol -- --disable-gpu --window-size=1920,1080
```

## Connect to Existing Instance

```typescript
import { ABPClient } from "agent-browser-protocol";

const client = new ABPClient("http://localhost:8222/api/v1");
const tabs = await client.tabs.list();
```

## API

The SDK mirrors the [ABP REST API](https://github.com/anthropics/anthropic-browser) 1:1:

| SDK Method | REST Endpoint |
|-----------|--------------|
| `client.browser.status()` | `GET /browser/status` |
| `client.browser.shutdown()` | `POST /browser/shutdown` |
| `client.tabs.list()` | `GET /tabs` |
| `client.tabs.create({ url })` | `POST /tabs` |
| `client.tabs.close(id)` | `DELETE /tabs/{id}` |
| `client.tabs.navigate(id, { url })` | `POST /tabs/{id}/navigate` |
| `client.tabs.click(id, { x, y })` | `POST /tabs/{id}/click` |
| `client.tabs.type(id, { text })` | `POST /tabs/{id}/type` |
| `client.tabs.keyPress(id, { key })` | `POST /tabs/{id}/keyboard/press` |
| `client.tabs.scroll(id, { x, y, delta_y })` | `POST /tabs/{id}/scroll` |
| `client.tabs.screenshot(id)` | `POST /tabs/{id}/screenshot` |
| `client.tabs.screenshotBinary(id)` | `GET /tabs/{id}/screenshot` |
| `client.tabs.execute(id, { script })` | `POST /tabs/{id}/execute` |
| `client.tabs.text(id)` | `POST /tabs/{id}/text` |
| `client.tabs.wait(id, { ms })` | `POST /tabs/{id}/wait` |
| `client.tabs.dialog(id)` | `GET /tabs/{id}/dialog` |
| `client.tabs.dialogAccept(id)` | `POST /tabs/{id}/dialog/accept` |
| `client.tabs.dialogDismiss(id)` | `POST /tabs/{id}/dialog/dismiss` |
| `client.tabs.execution(id)` | `GET /tabs/{id}/execution` |
| `client.tabs.setExecution(id, { paused })` | `POST /tabs/{id}/execution` |
| `client.downloads.list()` | `GET /downloads` |
| `client.downloads.get(id)` | `GET /downloads/{id}` |
| `client.downloads.cancel(id)` | `POST /downloads/{id}/cancel` |
| `client.fileChooser.provide(id, opts)` | `POST /file-chooser/{id}` |

## MCP Server

ABP includes a built-in MCP server. Configure in Claude Desktop:

```json
{
  "mcpServers": {
    "browser": {
      "transport": "streamable-http",
      "url": "http://localhost:8222/mcp"
    }
  }
}
```

## Environment Variables

| Variable | Description |
|---------|------------|
| `ABP_SKIP_DOWNLOAD=1` | Skip binary download during install |
| `ABP_BROWSER_PATH` | Path to a custom ABP binary |

## Platforms

- macOS (arm64, x64)
- Linux (x64)
- Windows (x64)
```

**Step 2: Commit**

```bash
git add tools/abp-npm/README.md
git commit -m "docs: add README for agent-browser-protocol npm package"
```

---

### Task 10: Final Verification

**Step 1: Clean build**

Run: `cd tools/abp-npm && rm -rf dist && npx tsup`
Expected: Build succeeds with no warnings

**Step 2: Typecheck**

Run: `cd tools/abp-npm && npx tsc --noEmit`
Expected: No errors

**Step 3: Verify package contents**

Run: `cd tools/abp-npm && npm pack --dry-run`
Expected: Lists files that would be included — `dist/` files and `browsers/` directory. No source files, no `node_modules/`.

**Step 4: Final commit with all accumulated changes**

```bash
git add tools/abp-npm/
git commit -m "feat: complete agent-browser-protocol npm package"
```
