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
