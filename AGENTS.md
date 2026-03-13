# AGENTS.md

## Cursor Cloud specific instructions

### Overview

darktable is a C/C++ photography workflow application built with CMake. It is a standalone desktop application (not a client-server architecture). The two main binaries are `darktable` (GTK 3 GUI) and `darktable-cli` (headless batch image processing).

### Building

See `README.md` "Building" section for full details. Quick reference:

```bash
# Ensure submodules are initialized
git submodule update --init

# Out-of-source build (in-source not allowed)
rm -rf build && mkdir build && cd build
CC=gcc CXX=g++ cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DUSE_OPENCL=OFF -DTESTBUILD_OPENCL_PROGRAMS=OFF ..
cmake --build . -j$(nproc)
sudo cmake --build . --target install
```

**Gotcha**: The default system compiler on Ubuntu 24.04 is Clang 18, which fails at link time with `cannot find -lstdc++`. Always pass `CC=gcc CXX=g++` to cmake to use GCC instead.

**Gotcha**: OpenCL is not available in the cloud VM (no GPU). Disable it with `-DUSE_OPENCL=OFF -DTESTBUILD_OPENCL_PROGRAMS=OFF`.

### Testing

- **Unit tests**: `cd build && ctest --output-on-failure` (uses cmocka framework). Tests are built when `-DBUILD_TESTING=ON`.
- **Lint**: Use `git clang-format --diff` to check formatting of changed files (not the whole codebase). See `CONTRIBUTING.md` and `.clang-format`.

### Running

- **CLI** (headless, for testing): `/opt/darktable/bin/darktable-cli <input> <output_dir>/ --out-ext jpg`
- **GUI** (needs display): `/opt/darktable/bin/darktable --disable-opencl`. Use `Xvfb :99 -screen 0 1280x1024x24 &` and `DISPLAY=:99` for headless environments.
- The `--configdir` flag is passed after `--core`, e.g. `--core --configdir /tmp/dt-config`.

### Key directories

- `src/` — main C/C++ source code
- `src/iop/` — image operation (processing) modules
- `src/tests/unittests/` — cmocka unit tests (see `src/tests/unittests/README.md`)
- `src/external/` — git submodules (rawspeed, LibRaw, OpenCL headers, etc.)
- `data/` — runtime data files (themes, icons, cameras.xml, etc.)
