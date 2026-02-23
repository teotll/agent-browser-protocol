// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

function getPackageRoot(): string {
  // Walk up from the running file to find the directory containing package.json.
  // This works regardless of bundle depth (dist/install.js vs dist/bin/abp.js).
  let dir: string;
  try {
    dir = path.dirname(fileURLToPath(import.meta.url));
  } catch {
    // CJS fallback: __dirname is injected by bundler
    dir = __dirname;
  }

  let current = dir;
  for (let i = 0; i < 5; i++) {
    if (fs.existsSync(path.join(current, "package.json"))) {
      return current;
    }
    const parent = path.dirname(current);
    if (parent === current) break;
    current = parent;
  }
  // Fallback: assume one level up (original behavior)
  return path.resolve(dir, "..");
}

const PACKAGE_ROOT = getPackageRoot();

export const ABP_VERSION = "0.1.3";
export const CHROME_VERSION = "146.0.7635.0";
export const GITHUB_REPO = "theredsix/agent-browser-protocol";

export interface PlatformInfo {
  platform: string;
  arch: string;
  archiveExt: string;
  executablePath: string;
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
  return `abp-${ABP_VERSION}-${info.platform}-${info.arch}${info.archiveExt}`;
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
