// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
  if (process.env.ABP_SKIP_DOWNLOAD === "1") {
    console.log("ABP_SKIP_DOWNLOAD=1, skipping binary download");
    return;
  }

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

  fs.mkdirSync(browsersDir, { recursive: true });

  await downloadToFile(url, archivePath);
  console.log(`Downloaded: ${archivePath}`);

  console.log("Extracting...");
  if (info.archiveExt === ".tar.gz") {
    execSync(`tar -xzf "${archivePath}" -C "${browsersDir}"`, {
      stdio: "inherit",
    });
  } else {
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

  fs.unlinkSync(archivePath);

  if (!fs.existsSync(execPath)) {
    throw new Error(`Extraction failed: executable not found at ${execPath}`);
  }

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
