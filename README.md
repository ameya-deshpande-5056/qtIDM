# qtIDM

qtIDM is a Linux-native Qt6 Widgets download manager written in C++20.

It is an original implementation. It does not ship copied IDM trademarks, icons, binary assets, fonts, proprietary UI resources, or reverse-engineered IDM internals.

## Features

### Download Engine

- Segmented HTTP, HTTPS, and FTP downloads through the libcurl multi-interface and Linux `epoll`.
- Up to 32 simultaneous connections per download with configurable per-host connection limits.
- Dynamic range planning that keeps connections occupied as queued ranges complete.
- Sparse `.part` files using `mmap` windows with a `pwrite` fallback.
- Atomic promotion from `.part` to the final destination after successful completion.
- Pause, cancel, and range-based resume with remote-size validation.
- Exponential retries for temporary transfer failures.
- Per-download speed limits, proxy configuration, HTTP headers, and username/password authentication.
- SHA-256 verification and rename, overwrite, or skip behavior for existing destinations.
- Optional executable and arguments launched after a successful download without invoking a shell.

### Streaming Media

- Non-DRM HLS and MPEG-DASH downloading through FFmpeg.
- Separate adaptive audio and video track downloading and muxing.
- Browser detection of HLS, DASH, direct audio/video, and common subtitle responses.
- Pause, resume, cancel, progress reporting, and persisted history for media downloads.

### Queues And Scheduling

- Persistent named queues with independent concurrency limits.
- Pause and resume dispatch for each queue without interrupting active downloads.
- Editable, removable, and reorderable pending entries with stable schedule IDs.
- Per-download start times, allowed weekdays, daily time windows, priorities, and repeat intervals.
- Queue settings and pending requests survive application restarts.

### Desktop Application

- Qt6 Widgets UI with category filtering, download properties, segment visualization, and system tray integration.
- Batch URL editor that creates multiple downloads without opening one dialog per URL.
- Clipboard monitoring for HTTP and HTTPS links.
- Replacement of expired source URLs while preserving downloaded ranges.
- SQLite download, segment, queue, and history persistence.
- Site grabber for same-origin links.
- Classic ZIP and ZIP64 central-directory preview.
- JSON history import and export.
- D-Bus single-instance URL forwarding.
- Dynamic light/dark theme tracking through the desktop portal with desktop-environment fallbacks.
- Persistent diagnostic logging in the Qt application-data directory.

### Browser Integration

- Chrome and Firefox extensions with native messaging integration.
- Browser-download interception that can be enabled or disabled from the popup.
- Context-menu capture for links, images, audio, and video.
- Cookies, referrer, and browser user agent forwarding.
- Captured-media list with per-item submission and clearing.
- Manual single-URL and batch submission of up to 100 addresses.
- Native application-side batch options for destination, queue, scheduling, connections, and conflict handling.

### Distribution

- Debian, AppImage, and Flatpak packaging routes.
- GitHub Actions build, test, package, smoke-test, artifact, and tagged-release workflow.

## Build

Linux only:

```bash
sudo apt-get install -y cmake ninja-build g++ qt6-base-dev qt6-base-dev-tools libcurl4-openssl-dev libsqlite3-dev pkg-config ffmpeg
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

The automated suite covers persistence, scheduling, queue controls, segmented and resumed transfers, retries, checksums, conflict policies, per-host limits, adaptive-media handling, ZIP/ZIP64 parsing, packaging metadata, and performance smoke tests. The downloader integration suite starts a localhost HTTP fixture server, so restricted sandboxes must permit localhost port binding.

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

Local package prerequisites on Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build g++ qt6-base-dev qt6-base-dev-tools libcurl4-openssl-dev libsqlite3-dev pkg-config debhelper-compat devscripts appstream flatpak flatpak-builder libfuse2 ffmpeg
```

Build and test the Debian package:

```bash
sh packaging/build-deb.sh
sudo apt-get install -y ./dist/qtidm_0.1.0-1_amd64.deb
qtIDM --version
```

Debian artifacts, including the optional debug-symbol `.ddeb`, are written to `dist/`.

Build and test the AppImage:

```bash
curl -L -o linuxdeploy.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -L -o linuxdeploy-plugin-qt.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
chmod +x linuxdeploy.AppImage linuxdeploy-plugin-qt.AppImage
sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy
sudo mv linuxdeploy-plugin-qt.AppImage /usr/local/bin/linuxdeploy-plugin-qt
APPIMAGE_EXTRACT_AND_RUN=1 sh packaging/build-appimage.sh
chmod +x ./*.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 ./*.AppImage --version
```

Build and test the Flatpak bundle:

```bash
sh packaging/release/version-info.sh
sh packaging/release/build-flatpak-bundle.sh
flatpak install --user -y dist/qtIDM-0.1.0.flatpak
flatpak run io.github.qtidm.qtidm --version
```

For release-equivalent local packaging with browser integration, set:

```bash
export QTIDM_CHROME_EXTENSION_ID=<your 32-character Chrome extension ID>
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
- release app packages use `QTIDM_CHROME_EXTENSION_ID` for the Chrome native host allowlist,
- the popup provides Captures, Add URL, and Settings views,
- media captures can be reviewed and submitted individually,
- manual batches are forwarded to one application-side batch editor,
- interception and media capture can be toggled independently.

Publishing browser extensions is a separate process. See:

```text
docs/BROWSER_EXTENSION_PUBLISHING.md
```

## Current Limitations

- DRM-protected media is intentionally unsupported.
- BitTorrent is not implemented.
- Browser extension end-to-end automation with installed Chrome and Firefox is not yet part of CTest.
- Chrome release builds require the published extension ID through `QTIDM_CHROME_EXTENSION_ID`.
- Flatpak does not install native messaging manifests into host browser directories; install a host package for browser integration.
- Site grabbing currently targets same-origin static links and does not render JavaScript applications.
- This project is IDM/FDM-inspired software, not a claim of complete feature or website parity.

## Logs

Application and native-host diagnostics are written to `qtidm.log` below each executable's Qt application-data directory. On Linux, logs can be located with:

```bash
find "${XDG_DATA_HOME:-$HOME/.local/share}" -name qtidm.log -print
```

Transfer failures are also shown in the download status and recorded with their download ID. Sensitive headers such as cookies should be removed before sharing logs.

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
