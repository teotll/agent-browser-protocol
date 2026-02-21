# ABP npm Skill Integration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add build-time skill generation to the ABP npm package so `claude plugin install agent-browser-protocol@npm` provides a usage guide skill alongside the MCP server.

**Architecture:** A Node.js script (`scripts/generate-skill.mjs`) reads the `kGuideContent` raw string from the C++ MCP handler source, prepends SKILL.md frontmatter, and writes to `skills/abp-browser/SKILL.md`. Wired into the npm `build` script via `prebuild`. The `package.json` `files` array includes `skills/` so the generated file ships with the published package.

**Tech Stack:** Node.js (ESM script), existing tsup build pipeline

**Design doc:** `docs/plans/2026-02-20-npm-skill-integration-design.md`

---

## Task 1: Create the skill generation script

**Files:**
- Create: `tools/abp-npm/scripts/generate-skill.mjs`

**Step 1: Create the script**

```javascript
#!/usr/bin/env node

// generate-skill.mjs
// Extracts kGuideContent from C++ source and generates SKILL.md

import { readFileSync, writeFileSync, mkdirSync, existsSync } from "node:fs";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const PACKAGE_ROOT = resolve(__dirname, "..");
const CPP_SOURCE = resolve(PACKAGE_ROOT, "../../chrome/browser/abp/abp_mcp_handler.cc");
const SKILL_DIR = resolve(PACKAGE_ROOT, "skills/abp-browser");
const SKILL_PATH = resolve(SKILL_DIR, "SKILL.md");

const FRONTMATTER = `---
name: abp-browser
description: Use when controlling a browser via ABP (Agent Browser Protocol). Provides tool usage guide, best practices, and debugging tips for the 12 MCP browser tools.
---

`;

function extractGuideContent(source) {
  // Match the kGuideContent raw string between R"md( and )md"
  const match = source.match(/kGuideContent\[\]\s*=\s*R"md\(([\s\S]*?)\)md"/);
  if (!match) {
    throw new Error("Could not find kGuideContent in C++ source");
  }
  return match[1];
}

function main() {
  // Try to read C++ source
  if (!existsSync(CPP_SOURCE)) {
    if (existsSync(SKILL_PATH)) {
      console.log(`[generate-skill] C++ source not found, using existing SKILL.md`);
      return;
    }
    console.error(`[generate-skill] ERROR: C++ source not found and no existing SKILL.md`);
    console.error(`  Expected: ${CPP_SOURCE}`);
    process.exit(1);
  }

  const source = readFileSync(CPP_SOURCE, "utf-8");
  const guideContent = extractGuideContent(source);

  // Write SKILL.md
  mkdirSync(SKILL_DIR, { recursive: true });
  writeFileSync(SKILL_PATH, FRONTMATTER + guideContent, "utf-8");
  console.log(`[generate-skill] Generated ${SKILL_PATH}`);
}

main();
```

**Step 2: Verify the script runs**

Run: `cd tools/abp-npm && node scripts/generate-skill.mjs`
Expected: Output `[generate-skill] Generated .../skills/abp-browser/SKILL.md`

**Step 3: Verify the generated SKILL.md**

Run: `head -10 tools/abp-npm/skills/abp-browser/SKILL.md`
Expected: Frontmatter followed by `# ABP Browser Control Guide`

**Step 4: Commit**

```bash
git add tools/abp-npm/scripts/generate-skill.mjs tools/abp-npm/skills/abp-browser/SKILL.md
git commit -m "feat(npm): add skill generation script extracting kGuideContent"
```

---

## Task 2: Wire generation into build and update package.json

**Files:**
- Modify: `tools/abp-npm/package.json`

**Step 1: Add `skills/` to files array and add prebuild script**

In `package.json`, make two changes:

1. Add `"skills/"` to the `files` array
2. Add `"generate-skill"` and `"prebuild"` scripts

The `files` array should become:
```json
"files": [
  "dist/",
  "browsers/",
  ".claude-plugin/",
  ".mcp.json",
  "skills/",
  "src/debug-ui.html"
]
```

The `scripts` section should become:
```json
"scripts": {
  "build": "tsup",
  "generate-skill": "node scripts/generate-skill.mjs",
  "prebuild": "node scripts/generate-skill.mjs",
  "postinstall": "node dist/install.js",
  "typecheck": "tsc --noEmit"
}
```

**Step 2: Verify prebuild runs during build**

Run: `cd tools/abp-npm && npm run build`
Expected: See `[generate-skill] Generated ...` before tsup output

**Step 3: Verify skills/ would be included in npm pack**

Run: `cd tools/abp-npm && npm pack --dry-run 2>&1 | grep skills`
Expected: `skills/abp-browser/SKILL.md` appears in the file list

**Step 4: Commit**

```bash
git add tools/abp-npm/package.json
git commit -m "feat(npm): wire skill generation into build, add skills/ to package files"
```

---

## Task 3: Verify end-to-end plugin structure

**Files:**
- Read: `tools/abp-npm/.claude-plugin/plugin.json`
- Read: `tools/abp-npm/.mcp.json`
- Read: `tools/abp-npm/skills/abp-browser/SKILL.md`

This task verifies the complete Claude plugin structure is correct.

**Step 1: Check plugin.json has correct name**

Run: `cat tools/abp-npm/.claude-plugin/plugin.json`
Expected: `"name": "agent-browser-protocol"` — this becomes the skill namespace

**Step 2: Check .mcp.json references the MCP proxy**

Run: `cat tools/abp-npm/.mcp.json`
Expected: MCP server config pointing to `dist/mcp-proxy.mjs`

**Step 3: Check SKILL.md has valid frontmatter**

Run: `head -5 tools/abp-npm/skills/abp-browser/SKILL.md`
Expected:
```
---
name: abp-browser
description: Use when controlling a browser via ABP...
---
```

**Step 4: Verify npm pack includes all plugin files**

Run: `cd tools/abp-npm && npm pack --dry-run 2>&1 | grep -E '(claude-plugin|mcp\.json|skills|dist/mcp)'`
Expected: All four paths appear:
- `.claude-plugin/plugin.json`
- `.mcp.json`
- `skills/abp-browser/SKILL.md`
- `dist/mcp-proxy.mjs` (or similar)

**Step 5: No commit needed — this is verification only**

---

## Task 4: Verify binary download works with published releases

**Step 1: Check the download URL resolves**

Run: `node -e "const {getDownloadUrl, getPlatformInfo} = await import('./tools/abp-npm/src/paths.ts'); console.log(getDownloadUrl(getPlatformInfo()))"`

If that doesn't work with raw TS, use:
```bash
cd tools/abp-npm && node -e "
import {getDownloadUrl, getPlatformInfo} from './dist/paths.js';
console.log(getDownloadUrl(getPlatformInfo()));
"
```

Expected: A valid GitHub Releases URL like `https://github.com/anthropics/anthropic-browser/releases/download/v0.1.1/abp-0.1.1-chrome-146.0.7635.0-mac-arm64.zip`

**Step 2: Verify the URL returns a valid response (HEAD request)**

Run: `curl -sIL "$(node -e "import {getDownloadUrl, getPlatformInfo} from './dist/paths.js'; console.log(getDownloadUrl(getPlatformInfo()));")" | head -5`
Expected: HTTP 200 (possibly after a 302 redirect from GitHub to the CDN)

**Step 3: Test postinstall in a clean directory**

Run:
```bash
mkdir /tmp/test-abp-install && cd /tmp/test-abp-install
npm init -y
npm install /path/to/tools/abp-npm
```

Expected: Postinstall downloads and extracts the binary. Output shows `ABP installed: ...`

**Step 4: Clean up**

Run: `rm -rf /tmp/test-abp-install`

**Step 5: No commit needed — this is verification only**
