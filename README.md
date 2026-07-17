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

For release builds with Chrome native messaging, pass the published Chrome extension ID:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQTIDM_CHROME_EXTENSION_ID=<chrome-extension-id>
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

## GitHub Actions

The repo uses one workflow:

```text
.github/workflows/build-release.yml
```

Normal push or pull request:

- installs dependencies,
- configures CMake,
- builds,
- runs the full CTest suite.

Weekly scheduled run:

- runs the normal build and tests,
- also runs the performance smoke test.

Manual workflow dispatch:

- runs build and tests,
- builds `.deb`, AppImage, and Flatpak,
- smoke-tests the packages,
- uploads workflow artifacts,
- does not publish a GitHub Release.

Version tag push:

- validates `QTIDM_CHROME_EXTENSION_ID`,
- runs build and tests,
- builds and smoke-tests packages,
- uploads artifacts,
- publishes the GitHub Release.

## Chrome Extension ID

For normal branch pushes, no Chrome extension ID is required. The workflow uses a development ID.

Before publishing a release tag, add the real Chrome Web Store extension ID as a GitHub Actions repository variable:

```text
GitHub repo -> Settings -> Secrets and variables -> Actions -> Variables -> New repository variable
```

```text
Name: QTIDM_CHROME_EXTENSION_ID
Value: <your 32-character Chrome extension ID>
```

Do not hardcode the published ID in source files. The workflow passes it to CMake during release builds.

## Browser Extensions

The workflow does not currently build Chrome Web Store or Firefox AMO upload packages.

Current behavior:

- browser extension source is kept under `browser/chrome` and `browser/firefox`,
- native messaging host manifests are generated during CMake configure,
- app packages include the extension source for manual/developer installation,
- release app packages use `QTIDM_CHROME_EXTENSION_ID` for the Chrome native host allowlist.

Publishing browser extensions is a separate process. See:

```text
docs/BROWSER_EXTENSION_PUBLISHING.md
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
