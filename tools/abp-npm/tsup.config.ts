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
