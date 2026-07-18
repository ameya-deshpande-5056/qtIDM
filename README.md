# qtIDM

qtIDM is a Linux-native Qt6 Widgets download manager written in C++20. It
combines segmented HTTP/HTTPS/FTP transfers, persistent queues, adaptive-media
handling, browser integration, and desktop automation in one application.

The project is independently developed and accepts only original contributions
or material distributed under compatible open-source licences.

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
- Shared HTTP/HTTPS/FTP session data quota with live usage and reset controls.
- Rename, overwrite, or skip behavior for existing destinations.
- Optional executable and arguments launched after a successful download without invoking a shell.
- Optional post-download move and automatic successful-entry cleanup.
- Safe automatic extraction of ZIP, 7z, RAR, and tar-family archives through 7-Zip, with path-traversal, symbolic-link, and hard-link rejection plus private staging before publication.
- Secret Service credential storage through `secret-tool`; vault-backed passwords are not serialized into download request JSON.
- MD5, SHA-1, SHA-256, and SHA-512 verification.

### Streaming Media

- Non-DRM HLS and MPEG-DASH downloading through FFmpeg.
- Explicit Widevine, PlayReady, FairPlay, and SAMPLE-AES detection with a clear refusal instead of attempting DRM circumvention.
- Separate adaptive audio and video track downloading and muxing.
- Maintained social-site extraction through `yt-dlp` for supported Instagram, Facebook, X/Twitter, TikTok, Reddit, Vimeo, Dailymotion, Twitch, and SoundCloud links.
- Browser detection of HLS, DASH, direct audio/video, and common subtitle responses.
- Pause, resume, cancel, progress reporting, and persisted history for media downloads.

### Queues And Scheduling

- Persistent named queues with independent concurrency limits.
- Pause and resume dispatch for each queue without interrupting active downloads.
- Editable, removable, and reorderable pending entries with stable schedule IDs.
- Per-download start times, allowed weekdays, daily time windows, priorities, and repeat intervals.
- Queue settings and pending requests survive application restarts.
- Configurable metered-network handling: allow downloads, hold new work, or pause active work until NetworkManager reports an unmetered connection.

### Desktop Application

- Qt6 Widgets UI with category filtering, download properties, segment visualization, and system tray integration.
- Search and status filtering, sortable columns, multi-row actions, automatic categories, file/folder actions, and desktop notifications.
- Batch URL editor that creates multiple downloads without opening one dialog per URL.
- Clipboard monitoring for HTTP and HTTPS links.
- Replacement of expired source URLs while preserving downloaded ranges.
- SQLite download, segment, queue, and history persistence.
- Same-origin site grabber with optional Chrome/Chromium JavaScript rendering, dynamic-link discovery, and rendered-DOM saving.
- Classic ZIP and ZIP64 central-directory preview.
- JSON history import and export.
- Newline-separated URL list import and export.
- D-Bus single-instance URL forwarding.
- Dynamic light/dark theme tracking through the desktop portal with desktop-environment fallbacks.
- Persistent diagnostic logging in the Qt application-data directory.

### Browser Integration

- Chrome and Firefox extensions with native messaging integration.
- Automated Chromium ZIP, signed Chrome CRX, and signed Firefox XPI packaging.
- Browser-download interception that can be enabled or disabled from the popup.
- Context-menu capture for links, images, audio, and video.
- Cookies, referrer, and browser user agent forwarding.
- Captured-media list with per-item submission and clearing.
- Manual single-URL and batch submission of up to 100 addresses.
- Native application-side batch options for destination, queue, scheduling, connections, and conflict handling.
- Chrome and Firefox end-to-end tests for interception and native-messaging context forwarding.

### Distribution

- Debian, AppImage, and Flatpak packaging routes.
- Signed browser-extension artifacts bundled into tagged application releases.
- GitHub Actions build, test, package, smoke-test, artifact, and tagged-release workflow.

## Runtime Integrations

The core application builds without these tools, but individual features use
them when available:

| Integration | Enables |
| --- | --- |
| FFmpeg | Non-DRM HLS/DASH downloading and adaptive-track muxing |
| 7-Zip (`7zz` or `7z`) | Automatic archive extraction |
| `secret-tool` from libsecret | Desktop Secret Service credential storage |
| `yt-dlp` | Maintained social-site format extraction |
| Chrome or Chromium | JavaScript-rendered site grabbing and Chrome extension testing |
| NetworkManager | Automatic metered-network state detection |

Missing optional tools are reported in the relevant dialog and do not prevent
ordinary HTTP, HTTPS, or FTP downloads.

## Build

Linux only:

```bash
sudo apt-get install -y cmake ninja-build g++ qt6-base-dev qt6-base-dev-tools libcurl4-openssl-dev libsqlite3-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Install all optional runtime integrations on Ubuntu with:

```bash
sudo apt-get install -y ffmpeg 7zip libsecret-tools yt-dlp chromium
```

For release builds with Chrome native messaging, pass the ID derived from the
persistent Chrome signing key:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DQTIDM_CHROME_EXTENSION_ID=<chrome-extension-id>
```

## Run

Start the locally built application:

```bash
./build/qtIDM
```

Common entry points:

- **Add URL** configures a direct download, credentials, scheduling, checksum,
  completion actions, and optional archive extraction.
- **Social** discovers selectable non-DRM formats on supported social sites.
- **Grabber** saves same-origin resources and can render pages through an
  installed Chrome/Chromium browser before link discovery.
- **Options** configures download defaults, session quotas, metered-network
  behavior, and archive automation.
- The Chrome and Firefox extensions can intercept downloads or submit captured
  media through the native messaging host.

For a system installation, use `sudo cmake --install build` after configuring
the desired native-messaging options.

## Test

```bash
ctest --test-dir build --output-on-failure
```

The automated suite covers persistence, scheduling, queue controls, segmented
and resumed transfers, retries, checksums, conflict policies, per-host limits,
metered-network dispatch, archive safety and extraction, credential-vault
transport, social metadata and DRM filtering, adaptive-media handling,
ZIP/ZIP64 parsing, packaging metadata, JavaScript-rendered site grabbing, and
performance smoke tests. The downloader and browser-extension integration
suites start localhost HTTP fixture servers, so restricted sandboxes must
permit localhost port binding.

Browser extension tests skip when their optional local tooling is unavailable; CI runs them strictly with Chrome for Testing plus Puppeteer, and Firefox plus `web-ext`:

```bash
python3 tests/browser_extension_e2e.py --browser chrome --strict
python3 tests/browser_extension_e2e.py --browser firefox --strict
```

Chrome 137 and newer branded builds no longer accept command-line unpacked extensions. Use Chrome for Testing or Chromium for the Chrome test.

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
sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev qt6-base-dev-tools libcurl4-openssl-dev libsqlite3-dev pkg-config debhelper-compat devscripts appstream desktop-file-utils flatpak flatpak-builder libfuse2 ffmpeg 7zip libsecret-tools yt-dlp
```

Build and test the Debian package:

```bash
sh packaging/build-deb.sh
sudo apt-get install -y ./dist/qtidm_0.1.1-1_amd64.deb
qtIDM --version
```

Debian artifacts, including the optional debug-symbol `.ddeb`, are written to `dist/`.
All packaging scripts use temporary workspaces and Flatpak state under `/tmp`
and clean them up on exit, so they do not create persistent `build-*` or
`.flatpak-builder` directories in the checkout.

Build and test the AppImage:

```bash
curl -L -o linuxdeploy.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -L -o linuxdeploy-plugin-qt.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage
printf '%s  %s\n' \
  e87ee0815d109282fdda73e34c2361d64d02b0ffaea3674b18f1fd1f6a687dcf linuxdeploy.AppImage \
  be1b7e166bf9975cfb694ebe6759ba40502ffc6196440d3e64aa90c4dbd67e9f linuxdeploy-plugin-qt.AppImage \
  | sha256sum --check --strict
chmod +x linuxdeploy.AppImage linuxdeploy-plugin-qt.AppImage
sudo mv linuxdeploy.AppImage /usr/local/bin/linuxdeploy
sudo mv linuxdeploy-plugin-qt.AppImage /usr/local/bin/linuxdeploy-plugin-qt
APPIMAGE_EXTRACT_AND_RUN=1 sh packaging/build-appimage.sh
APPIMAGE_EXTRACT_AND_RUN=1 ./dist/qtIDM-0.1.1-x86_64.AppImage --version
```

The script writes exactly one versioned AppImage to `dist/`. The checksums pin
the reviewed `continuous` linuxdeploy artifacts; when upstream replaces either
artifact, review the new binaries and update both this section and the workflow
checksums together.

Build and test the Flatpak bundle:

```bash
sh packaging/release/version-info.sh
sh packaging/release/build-flatpak-bundle.sh
flatpak install --user -y dist/qtIDM-0.1.1.flatpak
flatpak run io.github.qtidm.qtidm --version
```

The Flatpak targets the supported KDE 6.11 runtime, has write access only to the
user's Downloads directory plus portal-selected files, and bundles FFmpeg
through the runtime together with `secret-tool`, 7-Zip, and yt-dlp. The bundled
7-Zip and yt-dlp helpers currently target the x86-64 release architecture.

### Browser extension packages

Install the browser packaging tools in addition to the application build
dependencies:

```bash
sudo apt-get install -y zip unzip openssl nodejs npm
sudo npm install --global web-ext
```

Build development archives without signing credentials:

```bash
sh packaging/browser/build-extensions.sh \
  --mode development \
  --output build/browser-packages
```

This creates `qtidm-chrome.zip` for unpacked Chromium-family browser testing
and `qtidm-firefox-unsigned.xpi` for temporary Firefox testing.

For release-equivalent packaging with browser integration, set:

```bash
export QTIDM_CHROME_EXTENSION_ID=<your 32-character Chrome extension ID>
export QTIDM_CHROME_EXTENSION_KEY=/path/to/qtidm-chrome.pem
export QTIDM_CHROME_EXECUTABLE=/path/to/chrome
export WEB_EXT_API_KEY=<your AMO API key>
export WEB_EXT_API_SECRET=<your AMO API secret>
sh packaging/browser/build-extensions.sh --mode release
```

The release command creates a private-key-signed CRX and requests an unlisted,
Mozilla-signed XPI. Subsequent `.deb`, AppImage, and Flatpak builds bundle both
files from `browser/packages/`. Release packaging additionally requires a
Chrome/Chromium executable and `web-ext` 8 or newer.

### Versioning

The application and browser integrations have independent version tracks:

| Component | Current version | Authoritative files |
| --- | --- | --- |
| qtIDM application and release tag | `0.1.1` / `v0.1.1` | `CMakeLists.txt`, with release notes in `CHANGELOG.md` |
| Chrome and Firefox extensions | `0.3.2` | `browser/chrome/manifest.json` and `browser/firefox/manifest.json` |

The two checked-in browser manifest versions must always match each other, but
they do not need to match the application version. Signed CI builds derive a
higher four-part version from this three-part base version, the GitHub workflow
run number, and the run attempt. New runs and reruns therefore cannot reuse an
AMO version. Local signing builds still use the checked-in version and must bump
it manually before resubmission. Version changes do not alter the Chrome
extension ID, Firefox add-on ID, Chrome signing key, or AMO API credentials.

## GitHub Actions

The repo uses one workflow:

```text
.github/workflows/build-release.yml
```

Normal push or pull request:

- installs dependencies,
- lints and builds unsigned development extension packages,
- configures CMake,
- builds,
- runs the CTest suite,
- runs strict Chrome and Firefox extension end-to-end tests.

Weekly scheduled run:

- runs the normal build and tests,
- also runs the performance smoke test.

Manual workflow dispatch:

- runs build and tests,
- optionally builds signed browser packages when `sign_extensions` is selected,
- builds `.deb`, AppImage, and Flatpak,
- smoke-tests the packages,
- uploads workflow artifacts,
- does not publish a GitHub Release.

Version tag push:

- validates the Chrome signing key and extension ID,
- runs build and tests,
- builds a signed CRX and an unlisted Mozilla-signed XPI,
- bundles both extensions into every application package,
- builds and smoke-tests packages,
- uploads artifacts,
- publishes the GitHub Release.

## Browser Signing Configuration

For normal branch pushes, no Chrome extension ID is required. The workflow uses a development ID.

Generate the Chrome signing key once, keep it outside the repository, and
calculate its stable extension ID:

```bash
openssl genrsa -out qtidm-chrome.pem 2048
sh packaging/browser/chrome-extension-id.sh qtidm-chrome.pem
```

Before pushing a release tag, add that ID as a GitHub Actions repository variable:

```text
GitHub repo -> Settings -> Secrets and variables -> Actions -> Variables -> New repository variable
```

```text
Name: QTIDM_CHROME_EXTENSION_ID
Value: <your 32-character Chrome extension ID>
```

Add these repository secrets:

```text
QTIDM_CHROME_EXTENSION_KEY_B64 = <base64-encoded qtidm-chrome.pem>
WEB_EXT_API_KEY                = <AMO API key/JWT issuer>
WEB_EXT_API_SECRET             = <AMO API secret>
```

Create the key value with `base64 -w0 qtidm-chrome.pem`. Do not commit the
private key or upload it as an artifact. Firefox signing uses AMO's unlisted
self-distribution channel; it does not create a public add-on listing.

## Browser Extensions

Current behavior:

- browser extension source is kept under `browser/chrome` and `browser/firefox`,
- every CI run lints and builds development extension archives,
- tagged releases build `qtidm-chrome.crx` and `qtidm-firefox.xpi`,
- signed extension files are included in the `.deb`, AppImage, and Flatpak,
- the `.deb` registers the bundled CRX as an external Google Chrome extension,
- the signed XPI remains available for user installation from a file,
- native messaging host manifests are generated during CMake configure,
- app packages include the extension source for manual/developer installation,
- release app packages use `QTIDM_CHROME_EXTENSION_ID` for the Chrome native host allowlist,
- the popup provides Captures, Add URL, and Settings views,
- media captures can be reviewed and submitted individually,
- manual batches are forwarded to one application-side batch editor,
- interception and media capture can be toggled independently,
- Firefox extension `0.3.2` requires Firefox 140 or newer for Mozilla's built-in
  data-collection consent declaration,
- real Chrome and Firefox automation verifies download interception through a temporary native host.

Extension signing, private-key handling, and installation boundaries are documented in:

[Browser Extension Packaging and Private Distribution](docs/BROWSER_EXTENSION_PUBLISHING.md).

### Installing bundled extensions

The release `.deb` installs the signed files at:

```text
/usr/share/qtidm/browser/packages/qtidm-chrome.crx
/usr/share/qtidm/browser/packages/qtidm-firefox.xpi
```

- The release `.deb` also installs an external-extension descriptor for Google
  Chrome. Restart Chrome after installing the package; users can still disable
  or uninstall the extension.
- In Firefox, open Add-ons and Themes, select **Install Add-on From File**, and
  choose `qtidm-firefox.xpi`. Standard Firefox requires this Mozilla-signed
  release XPI; the unsigned development XPI is only for temporary testing.
- AppImage and Flatpak releases contain the files but do not modify host-browser
  configuration. Their internal locations are
  `/usr/share/qtidm/browser/packages` and `/app/share/qtidm/browser/packages`,
  respectively. Use the `.deb` or a separate host-side integration installer
  when native messaging is required.
- Flatpak cannot expose its sandboxed native host directly to a host-installed
  browser.

## Current Limitations

- DRM-protected media is intentionally unsupported.
- YouTube-specific extraction is intentionally unavailable; browser capture and ordinary non-DRM media downloading remain available.
- BitTorrent is not implemented.
- Chrome release builds require the signing-key-derived extension ID through `QTIDM_CHROME_EXTENSION_ID`.
- Flatpak does not install native messaging manifests into host browser directories; install a host package for browser integration.
- JavaScript-rendered site grabbing requires an installed Chrome/Chromium-family browser; static grabbing remains available as a fallback.
- Website-specific extraction and browser interception can be affected by site or browser changes.

## Logs

Application and native-host diagnostics are written to `qtidm.log` below each executable's Qt application-data directory. On Linux, logs can be located with:

```bash
find "${XDG_DATA_HOME:-$HOME/.local/share}" -name qtidm.log -print
```

Transfer failures are also shown in the download status and recorded with their download ID. Sensitive headers such as cookies should be removed before sharing logs.

## Release

Prepare application and extension changes before starting the release commit.
Code changes should already be committed; the release commit should contain
only version and release-metadata updates.

1. Update the project version in `CMakeLists.txt` and the release notes in
   `CHANGELOG.md`.
2. Review the three-part base versions in `browser/chrome/manifest.json` and
   `browser/firefox/manifest.json`. They must match each other and are
   independent of the project version. The release workflow automatically
   derives a unique, increasing signed-package version; increment the base
   manually only when starting a new extension release line or signing locally.
3. Update the current-version table in this README.
4. Confirm the signing variable and secrets described above are configured.
5. Build and test the release candidate before committing:

```bash
VERSION="$(sh packaging/release/version-info.sh)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Review and create the release commit with an explicit file list so the Chrome
private key and unrelated working-tree files cannot be staged accidentally:

```bash
VERSION="$(sh packaging/release/version-info.sh)"
git status --short
git add CMakeLists.txt CHANGELOG.md README.md \
  browser/chrome/manifest.json browser/firefox/manifest.json
git diff --cached --check
git diff --cached
git commit -m "Prepare v${VERSION} release"
```

Create an annotated tag only after the release commit exists, then push the
commit before the tag:

```bash
VERSION="$(sh packaging/release/version-info.sh)"
git tag -a "v$VERSION" -m "qtIDM $VERSION"
git push origin main
git push origin "v$VERSION"
```

The single workflow `.github/workflows/build-release.yml` builds, tests, packages, smoke-tests, uploads artifacts, and publishes the GitHub Release for version tags.
Treat pushed release tags as immutable. If a published tag points at the wrong
commit, prefer preparing a new patch release instead of force-moving the tag.

## Documentation

- [Test plan](docs/TEST_PLAN.md)
- [Linux differences](docs/LINUX_DIFFERENCES.md)
- [Browser extension packaging and private distribution](docs/BROWSER_EXTENSION_PUBLISHING.md)
- [Capability matrix](docs/CAPABILITIES.md)
