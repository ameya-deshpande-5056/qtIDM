# qtIDM

qtIDM is a Linux-native Qt6 Widgets download manager written in C++20.

It is an original implementation. It does not ship copied IDM trademarks, icons, binary assets, fonts, proprietary UI resources, or reverse-engineered IDM internals.

## Features

- Qt6 Widgets desktop UI.
- Linux-only C++20 codebase.
- libcurl multi-interface integrated with Linux `epoll`.
- Segmented HTTP/HTTPS/FTP downloads, up to 32 segments.
- Sparse file writes with `mmap` windows and `pwrite` fallback.
- SQLite download, segment, queue, and history persistence.
- Resume validation for changed remote sizes.
- Scheduler, speed limit, proxy, and authentication fields.
- Site grabber.
- ZIP and ZIP64 preview.
- JSON import/export.
- Chrome and Firefox native messaging integration source.
- D-Bus single-instance handling.
- Portal D-Bus theme detection.
- `.deb`, AppImage, and Flatpak packaging.
- GitHub Actions build, test, package, smoke-test, and release workflow.

## Build

Linux only:

```bash
sudo apt-get install -y cmake ninja-build g++ qt6-base-dev qt6-base-dev-tools libcurl4-openssl-dev libsqlite3-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Performance smoke tests run in CI on the weekly schedule. To run them locally:

```bash
QTIDM_RUN_PERF_TESTS=1 ctest --test-dir build -R qtIDM_performance_smoke_tests --output-on-failure
```

## Packaging

```bash
sh packaging/build-deb.sh
sh packaging/build-appimage.sh
sh packaging/build-flatpak.sh
```

## Release

Update `CMakeLists.txt` and `CHANGELOG.md`, then tag with the matching version:

```bash
git tag v0.1.0
git push origin main --tags
```

The single workflow `.github/workflows/build-release.yml` builds, tests, packages, smoke-tests, uploads artifacts, and publishes the GitHub Release for version tags.

## Documentation

- `docs/TEST_PLAN.md`
- `docs/LINUX_DIFFERENCES.md`
- `docs/BROWSER_EXTENSION_PUBLISHING.md`
