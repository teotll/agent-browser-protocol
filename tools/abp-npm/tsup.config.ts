import { defineConfig } from "tsup";

export default defineConfig({
  entry: {
    index: "src/index.ts",
    install: "src/install.ts",
    "bin/abp": "src/bin/abp.ts",
    "bin/debug": "src/bin/debug.ts",
    "mcp-proxy": "src/mcp-proxy.ts",
  },
  format: ["esm", "cjs"],
  dts: { entry: "src/index.ts" },
  clean: true,
  splitting: false,
  sourcemap: true,
});
