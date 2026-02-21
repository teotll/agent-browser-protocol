# Building ABP from Source

This guide covers building the Agent Browser Protocol (ABP) from Chromium source on macOS, Linux, and Windows.

## Prerequisites

### All platforms

Install Google's **depot_tools** build toolchain:

```bash
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git ~/depot_tools
export PATH="$HOME/depot_tools:$PATH"
```

Add the PATH export to your shell profile (`~/.bashrc`, `~/.zshrc`, etc.) to make it permanent.

### macOS

- **Xcode** -- install the latest version from the App Store
- **macOS 13** (Ventura) or later

### Linux (Ubuntu/Debian)

Run the Chromium dependency installer:

```bash
sudo ./build/install-build-deps.sh --no-prompt
```

### Windows

- **Visual Studio 2022** with the "Desktop development with C++" workload
- **Windows 10 SDK** or **Windows 11 SDK** (installed via the Visual Studio Installer)

---

## Clone & Sync

### 1. Clone the ABP repository

```bash
mkdir -p ~/src && cd ~/src
git clone <ABP_REPO_URL> chromium
```

### 2. Create the gclient workspace file

Create `~/src/.gclient` with the following content:

```python
solutions = [
  {
    "name": "src",
    "url": "<ABP_REPO_URL>",
    "managed": False,
    "custom_deps": {},
  },
]
```

### 3. Create the `src` symlink

gclient expects the source tree at `~/src/src`. Create a symlink pointing to the `chromium` directory:

```bash
cd ~/src
ln -s chromium src
```

### 4. Sync dependencies

```bash
cd ~/src
gclient sync --no-history
```

This downloads all third-party dependencies. It takes a while on the first run.

### Directory structure

After setup, the workspace looks like this:

```
~/src/
├── .gclient              # gclient configuration
├── src -> chromium       # symlink required by gclient
└── chromium/             # ABP source code
    ├── chrome/browser/abp/   # ABP implementation
    ├── tools/abp/            # Build, package, and release scripts
    ├── tools/abp-npm/        # npm package (version source)
    ├── out/                  # Build output directories
    └── plans/                # Design documentation
```

---

## Configure

All `gn gen` commands should be run from the Chromium source root (`~/src/chromium` or equivalently `~/src/src`).

### Debug build (recommended for development)

A debug component build gives the fastest incremental compile times:

```bash
gn gen out/Default --args='is_debug=true is_component_build=true symbol_level=1 dcheck_always_on=true'
```

### Release builds

Release builds are statically linked with full optimizations and no debug symbols.

**macOS arm64:**

```bash
gn gen out/Release-arm64 --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 target_cpu="arm64"'
```

**macOS x64:**

```bash
gn gen out/Release-x64 --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 target_cpu="x64"'
```

**Linux:**

```bash
gn gen out/Release --args='is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0'
```

**Windows:**

```
gn gen out/Release --args="is_debug=false is_component_build=false symbol_level=0 is_official_build=true chrome_pgo_phase=0 enable_resource_allowlist_generation=false"
```

Note the additional `enable_resource_allowlist_generation=false` flag required on Windows.

---

## Build

Always use `autoninja` (not `ninja` directly) -- it automatically selects the optimal parallelism for your machine.

### Debug build

```bash
autoninja -C out/Default chrome
```

### Release build -- macOS and Linux

```bash
autoninja -C out/Release chrome          # Linux
autoninja -C out/Release-arm64 chrome    # macOS arm64
autoninja -C out/Release-x64 chrome      # macOS x64
```

### Release build -- Windows

```
autoninja -C out/Release abp
```

On Windows the build target is `abp` rather than `chrome`.

### Build times

| | First build | Incremental |
|---|---|---|
| Typical | 4--6 hours | Seconds to minutes |

The first full build is slow. After that, incremental builds only recompile changed files and are much faster.

---

## Run

### macOS (debug)

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --enable-abp
```

### macOS (release)

```bash
./out/Release-arm64/ABP.app/Contents/MacOS/ABP --enable-abp
```

### Linux

```bash
./out/Default/abp --enable-abp        # debug
./out/Release/abp --enable-abp        # release
```

### Windows

```
.\out\Release\abp.exe --enable-abp
```

### Verify

Once ABP is running, confirm the REST API is available:

```bash
curl http://localhost:8222/api/v1/browser/status
```

You should get a JSON response indicating the browser is ready.

### Optional flags

| Flag | Description |
|---|---|
| `--abp-port=PORT` | Listen on a custom port (default: 8222) |
| `--abp-session-dir=DIR` | Store session data (SQLite DB, screenshots) in a specific directory |

Example with session directory:

```bash
./out/Default/ABP.app/Contents/MacOS/ABP --enable-abp --abp-session-dir=sessions/$(date +%Y%m%d_%H%M%S)
```

---

## Packaging

Packaging scripts create distributable archives from a completed release build. The version is read automatically from `tools/abp-npm/package.json`.

### macOS

```bash
./tools/abp/package-mac.sh
```

Control the target architecture with the `BUILD_ARCH` environment variable:

```bash
BUILD_ARCH=arm64     ./tools/abp/package-mac.sh   # arm64 only
BUILD_ARCH=x64       ./tools/abp/package-mac.sh   # x64 only
BUILD_ARCH=universal ./tools/abp/package-mac.sh   # universal binary
BUILD_ARCH=all       ./tools/abp/package-mac.sh   # arm64 + universal (default)
```

Output: `dist/abp-{VERSION}-mac-{arch}.zip`

### Linux

```bash
./tools/abp/package-linux.sh
```

Output: `dist/abp-{VERSION}-linux-x64.tar.gz`

The Linux archive includes the binary, shared libraries, locale files, resource paks, ICU data, and V8 snapshots.

### Windows

```powershell
.\tools\abp\package-win.ps1
```

Output: `dist\abp-{VERSION}-win-x64.zip`

---

## Platform Notes

### macOS

- **Universal binaries** are created by building arm64 and x64 separately, then merging with `chrome/installer/mac/universalizer.py`.
- **Code signing and notarization** are handled by the release script (`tools/abp/release-mac.sh`). Set `SIGNING_IDENTITY`, `NOTARIZE_KEY`, `NOTARIZE_KEY_ID`, and `NOTARIZE_ISSUER` environment variables, or use `SKIP_SIGNING=1` / `SKIP_NOTARIZATION=1` to skip.
- The full release pipeline (build, validate, sign, notarize, package) is:
  ```bash
  BUILD_ARCH=all ./tools/abp/release-mac.sh
  ```

### Linux

- The packaged archive is a self-contained directory with the binary and all required runtime files.
- For headless environments (CI, servers), ensure a display is available or pass `--headless` to run without a visible window.
- The full release pipeline (build, validate, package) is:
  ```bash
  ./tools/abp/release-linux.sh
  ```

### Windows

- Builds use the PowerShell script `tools/abp/build-win.ps1`.
- The GN args include `enable_resource_allowlist_generation=false`, which is required for the Windows build to succeed.
- The build target is `abp` (not `chrome`) on Windows.
- The full release pipeline (build, validate, package) is:
  ```powershell
  .\tools\abp\release-win.ps1
  ```

---

## Troubleshooting

**`gclient sync` fails:** Make sure the `src` symlink exists and points to `chromium`. gclient requires this exact layout.

**Build errors after pulling changes:** Run `gclient sync` again to update third-party dependencies.

**Slow builds:** Use `is_component_build=true` for debug builds. This produces shared libraries instead of a single monolithic binary, dramatically reducing link times.

**"autoninja not found":** Ensure `depot_tools` is on your PATH.
