---
name: darktable-cloud-starter
description: Practical quickstart for Cloud agents working in darktable; use this when you need to build, run, smoke test, or choose the right test loop for a code area.
---

# Darktable Cloud Starter

Use this skill when you need the fastest safe path to build, run, and test
`darktable` in a Linux cloud agent.

## What matters first

- No normal app login is required. The only credentials in this repo are for
  optional macOS signing/notarization, not day-to-day dev work.
- This is a desktop C/C++ app, not a web service. The highest-signal smoke test
  is usually `darktable-cli`, then a manual GUI check if you touched UI or image
  pipeline behavior.
- Keep your runs isolated. Do not reuse a real darktable config or library when
  testing agent changes.
- Do not do in-source builds. Use `build/` or another out-of-tree build dir.

## First-time setup

1. Make sure submodules exist:

   `git submodule update --init`

2. If you need the full Linux dependency set, use the package list from
   `.github/workflows/ci.yml` as the source of truth. It is more reliable for
   cloud agents than ad-hoc package guesses.

3. Prefer a local install prefix to avoid `sudo` churn in cloud environments:

   `./build.sh --prefix "$PWD/install" --build-generator Ninja --build-type Debug --install`

4. Verify the binaries exist before deeper testing:

   `./install/bin/darktable --version || true`

   `./install/bin/darktable-cli --version || true`

## Runtime isolation and "feature flag" knobs

There is no remote feature-flag service in this repo. The practical flags here
are CMake/build toggles and temporary runtime overrides:

- Build toggles:
  - `-DBUILD_TESTING=ON` to build unit tests.
  - `-DUSE_AI=ON` or `OFF` for AI-backed features.
  - `-DUSE_OPENCL=OFF` or runtime `--disable-opencl` when stability matters more
    than GPU coverage.
  - `-DUSE_LUA=OFF` if Lua/plugin support is unrelated and causing dependency
    churn.
- Temporary runtime overrides:
  - `--configdir "$PWD/.tmp/dt-config"`
  - `--cachedir "$PWD/.tmp/dt-cache"`
  - `--library "$PWD/.tmp/dt-library.db"` or `--library :memory:`
  - `--conf write_sidecar_files=never`
  - `--conf host_memory_limit=8192`
  - `--conf worker_threads=4`

Good default isolation command for agent runs:

`mkdir -p .tmp/dt-config .tmp/dt-cache && ./install/bin/darktable --configdir "$PWD/.tmp/dt-config" --cachedir "$PWD/.tmp/dt-cache" --library "$PWD/.tmp/dt-library.db" --conf write_sidecar_files=never --disable-opencl`

If you open real images in the GUI, also set Preferences -> Storage -> XMP
sidecar files -> `never`.

## Codebase area playbooks

### 1) Core app / GUI / image pipeline

Use for changes in `src/common`, `src/develop`, `src/iop`, `src/libs`, `data`,
and most UI-facing code.

Recommended loop:

1. Rebuild incrementally:

   `cmake --build build -j"$(nproc)"`

2. Run a shell smoke test first:

   `./install/bin/darktable-cli --width 1000 --height 1000 --hq true --apply-custom-presets false data/pixmaps/256x256/darktable.png output.png --core --disable-opencl --conf host_memory_limit=8192 --conf worker_threads=4 -t 4 --conf plugins/lighttable/export/force_lcms2=FALSE --conf plugins/lighttable/export/iccintent=0`

3. If the change is UI-visible or easier to validate interactively, launch the
   GUI with isolated state:

   `./install/bin/darktable --configdir "$PWD/.tmp/dt-config" --cachedir "$PWD/.tmp/dt-cache" --library "$PWD/.tmp/dt-library.db" --conf write_sidecar_files=never --disable-opencl`

4. If the app crashes or hangs early, retry with:

   `./install/bin/darktable --disable-opencl -d common -d opencl -d perf`

When to use manual GUI testing:

- You changed `.c` code that affects widgets, layout, module visibility, or
  interactive image editing behavior.
- You touched an `iop` and need to confirm the visible output, not just that the
  export path still runs.

### 2) CLI/export path

Use for changes in `src/cli`, export modules, image IO, or anything CI would
exercise through `darktable-cli`.

Fastest high-signal workflow:

1. Build/install:

   `./build.sh --prefix "$PWD/install" --build-generator Ninja --build-type Debug --install`

2. Run the CI-style export smoke test:

   `./install/bin/darktable-cli --width 1000 --height 1000 --hq true --apply-custom-presets false data/pixmaps/256x256/darktable.png output.png --core --disable-opencl --conf host_memory_limit=8192 --conf worker_threads=4 -t 4 --conf plugins/lighttable/export/force_lcms2=FALSE --conf plugins/lighttable/export/iccintent=0`

3. Confirm `output.png` was written and re-run with your bug repro input if you
   have one.

Notes:

- `darktable-cli` is the safest first test in cloud because it avoids the GUI.
- The CLI already forces ephemeral behavior internally for some settings, so it
  is well suited for agent smoke tests.

### 3) Unit tests

Use for changes with existing cmocka coverage or for library-style logic that
does not need a full image regression run.

Setup:

`./build.sh --prefix "$PWD/install" --build-generator Ninja --build-type Debug -- -DBUILD_TESTING=ON`

Targeted workflow:

1. Build one suite:

   `cmake --build build --target test_<suite-name> -j"$(nproc)"`

2. Run that suite directly for full output:

   `./build/src/tests/unittests/test_<suite-name>`

3. Or run through ctest:

   `cd build && ctest --output-on-failure -R <suite-name>`

Broader workflow:

`cd build && ctest --output-on-failure`

Use this area first for:

- `src/common`
- `src/iop` algorithm changes with existing unit coverage
- `src/ai` backend tests

### 4) Integration tests

Use for image-pipeline regressions where exact output matters.

Prereqs:

- The integration tests are a submodule. If missing:
  `git submodule update --init src/tests/integration`
- The runner needs `darktable-cli`.
- The runner also expects ImageMagick `compare`; some workflows also mention
  `zopflipng`.

Recommended workflow:

1. Install or point to your test binary:

   `export DARKTABLE_CLI="$PWD/install/bin/darktable-cli"`

2. Run a single test first:

   `cd src/tests/integration && ./run.sh --disable-opencl 0001-exposure`

3. Then run the relevant subset or full suite:

   `cd src/tests/integration && ./run.sh --disable-opencl --fast-fail`

Useful knobs:

- `--disable-opencl` for reproducibility.
- `--no-deltae` for a lighter run when Delta-E tooling is unavailable.
- `--op=<n>` to run tests matching an operation ID.

Use this area first for:

- `src/iop`
- color / pipeline regressions
- raw import / export behavior

### 5) AI-backed features

Use for changes in `src/ai` or modules that depend on ONNX Runtime-backed
behavior.

Setup:

`./build.sh --prefix "$PWD/install" --build-generator Ninja --build-type Debug --enable-ai -- -DBUILD_TESTING=ON`

Targeted workflow:

1. Build the backend test:

   `cmake --build build --target test_ai_backend -j"$(nproc)"`

2. Run it with verbose output:

   `cd build && ctest -R test_ai_backend -V`

3. If you need to test local/mock models, remember model discovery respects
   `--configdir`; place models under:

   `"$PWD/.tmp/dt-config/models/"`

Notes:

- If ONNX Runtime is missing, the build system may auto-download it unless the
  environment is explicitly offline.
- If AI is unrelated to your change, disable it and keep the loop simpler.

### 6) Packaging / platform-specific work

Use only when you touch `packaging/**` or release-oriented scripts.

- Linux cloud agents are best for validating shared build logic first, not for
  pretending to be every packaging target.
- For Windows packaging, start with `packaging/windows/README.md`.
- For macOS packaging, start with `packaging/macosx/BUILD_hb.txt`.
- macOS notarization/signing needs credentials; do not treat that as a normal
  blocker for unrelated code changes.

Practical validation loop from Linux:

1. Re-run the normal local build/install.
2. Validate the packaging script or doc you touched.
3. If your change only affects packaging docs, a build + command sanity check is
   usually enough.

## Common cloud-agent tricks

- Prefer `darktable-cli` before GUI work. It is cheaper, faster, and matches CI.
- Prefer `--disable-opencl` on first repro; add GPU coverage only after the CPU
  path is stable.
- Use isolated `--configdir`, `--cachedir`, and `--library` every time.
- For lens-correction issues, update the local Lensfun database:

  `lensfun-update-data`

- If submodules or optional deps are missing, check `.github/workflows/ci.yml`
  before inventing new setup steps.

## How to update this skill

When you discover a new runbook trick, add it here immediately if it saves the
next cloud agent time.

Keep updates small and practical:

1. Put the note under the nearest codebase area.
2. Include an exact copy-paste command, not just prose.
3. Say when to use it and whether it is a workaround, default path, or
   environment fix.
4. Prefer CI-backed commands or commands already documented in this repo.
5. If the trick only matters in cloud environments, say so explicitly.
