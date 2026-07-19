# Changelog

## 0.2.1

- Replaced runtime SVG toolbar and application icons with embedded PNG assets so icons render consistently even when a Linux Qt package omits the SVG image plugin.
- Regenerated the application, desktop, tray, README, About dialog, and browser-extension logo variants in the dark wine brand palette, removing the retired royal-blue identity from shipped assets.
- Bumped the Chrome and Firefox extensions to version 0.4.0 so browsers install the redesigned popup, stylesheet, and icon assets as an update instead of continuing to use cached 0.3.2 files.
- Strengthened browser-package tests to inspect generated ZIP and XPI contents for the current dark stylesheet, popup logo, toolbar icons, and brand colors.
- Added resource tests that require PNG application and action icons and prevent accidental fallback to runtime SVG resources.

## 0.2.0

- Added a permanent session dashboard to the bottom status bar with combined download speed, data downloaded during the current session, and an alternate per-download speed-limit control.
- Added persistent alternate-limit presets and custom limits that apply immediately to active downloads and automatically to new or resumed transfers.
- Fixed segmented downloads multiplying their configured speed limit by the number of active connections; limits now cap the aggregate rate of each download.
- Replaced volatile instantaneous speed and ETA values with a smoothed recent download rate, stable ETA calculation, resume-safe sampling, and explicit stalled or unavailable states.
- Added exact session-byte accounting across regular and media progress updates without recounting pre-existing partial data after resume.
- Replaced host-dependent theme icons with a complete embedded SVG action-icon set, preventing missing toolbar icons on minimal distributions and keeping Debian, Flatpak, and AppImage builds visually consistent.
- Added distinct semantic colors for action icons while reserving the application brand color exclusively for qtIDM identity assets.
- Added dynamic pointing-hand and forbidden cursors for enabled and disabled toolbar icon buttons.
- Restored the simplified download-arrow logo, changed the brand from royal blue to a dark wine palette, and propagated it to the application window, dialogs, About dialog, system tray, README, desktop metadata, and browser extensions.
- Added reproducible 16–512 px application icons and browser-toolbar variants generated from the canonical SVG.
- Added a permanently dark Chrome and Firefox extension theme using qtIDM's wine branding and warm copper accents.
- Changed the application ID to `io.qtidm.Qtidm` across Flatpak, AppStream, desktop files, icons, D-Bus, runtime association, packaging, and CI.
- Expanded packaging and integration tests to cover application identity, static icon completeness, extension branding, generated icon sizes, native-host metadata, and aggregate segmented-transfer throttling.

## 0.1.8

- Fixed Flatpak builds with stricter Qt headers by including the complete JSON array type used by single-instance browser handoffs.
- Fixed Firefox browser-download forwarding so suggested filenames from the browser are preserved without synthetic collision suffixes.
- Fixed browser-originated signed and encoded URLs being decoded too early, which could make libcurl reject valid URLs as malformed.
- Added a persistent bottom properties pane inspired by qBittorrent, with General, Request, and Segments tabs for inspecting selected downloads without opening a blocking edit dialog.
- Updated toolbar, menu, and context-menu actions so start, pause, stop, delete, and edit are enabled only when the current download selection supports them.
- Tightened the main splitter layout so the category pane, download list, and details pane no longer leave a large empty gap between panes.
- Expanded the About dialog with the application version, UTC build time, Qt runtime version, and project GitHub URL.

## 0.1.6

- Fixed structured browser downloads and captured HLS/DASH requests being decoded as empty D-Bus maps, which caused valid URLs to be rejected with a batch-download warning.
- Prevented failed browser handoffs from repeatedly intercepting their own restored downloads, and made browser-originated dialogs non-reentrant so malformed or repeated requests cannot create an uncontrollable modal popup storm.

## 0.1.5

- Reworked browser native messaging into a persistent, request-correlated session with explicit desktop acceptance before a browser download is removed; failed handoffs restore or resume the browser download.
- Added browser forwarding for POST downloads, including form data and raw/binary request bodies up to 4 MiB.
- Added one shared application-side options dialog for browser batches while preserving each URL's cookies, headers, filename, media hint, and POST context.
- Added context-menu collection for page links/images and selected links, bounded to 100 URLs per batch.
- Added browser interception policies for included/excluded extensions, minimum size, pause, current-site exclusion, and configurable Alt-key bypass.
- Added explicit `qtidm:` link handling and bounded per-tab capture of HLS, DASH, direct media, and subtitle requests.
- Strengthened real Chrome and Firefox E2E tests to require a persistent native-messaging prepare/download session on the same host process; CI now fails if either E2E test is missing.
- Updated CI browser tooling to resolve the current active Node.js LTS automatically.
- Browser downloads now use the resolved `Content-Disposition` filename, discard browser-generated collision suffixes such as `(1)` or `(2)`, and remove the browser's temporary file before qtIDM opens its download-options dialog; qtIDM adds numbered suffixes only for real destination or active `.part` conflicts.
- Chrome release packaging now publishes both a stable-ID unpacked ZIP for branded Chrome and a developer-signed CRX for compatible Chromium variants and managed Linux installation, avoiding `CRX_REQUIRED_PROOF_MISSING` as the only GitHub installation path.
- New single-URL downloads default to category folders in `~/Downloads` (such as Videos, Music, Images, Programs, Documents, Compressed, and Others), creating the selected destination folder when the download is confirmed.
- Closing the main window now keeps qtIDM running in the system tray; active downloads prompt to keep running, quit, or cancel the close.

## 0.1.4

- Added collision-free browser-extension version generation for signed CI runs and reruns.
- Added tagged-release version synchronization when a higher GitHub tag is ahead of `CMakeLists.txt`.
- Fixed Linux native-messaging manifest installation so Chrome, Chromium, and Firefox can find the qtIDM host.
- Browser interception now removes successfully redirected items from browser download history and resumes the browser download if qtIDM is unavailable.
- Captured HLS/DASH requests now retain their browser request context in session storage and route extensionless manifests through FFmpeg.
- Browser redirects now retain resolved filenames, while adaptive-media requests serialize header capture and pass browser User-Agent and Referer through FFmpeg's dedicated HTTP options.
- Removed empty theme-watcher and invalid fully-decoded URL warnings, and stopped duplicate empty failure reports after HTTP errors.

## 0.1.1

- Added generic MD5, SHA-1, SHA-256, and SHA-512 verification.
- Persisted complete request options so resumed downloads retain headers, credentials, proxy, checksum, timeout, retry, and automation settings.
- Added configurable concurrency, retry, timeout, redirects, proxy, User-Agent, and HTTP/FTP session data-quota defaults.
- Added search, status filters, sorting, automatic categories, size/speed/ETA display, multi-row actions, file actions, desktop notifications, and richer property editing.
- Added text-link import/export, multi-link clipboard capture, recursive grabber depth, post-download move/list cleanup, and completion-command placeholders.
- Added browser-capture host and file-extension exclusions.
- Added JavaScript-rendered same-origin site grabbing with rendered-DOM saving, dynamic resource discovery, and static fallback.
- Added real Chrome and Firefox extension end-to-end tests for browser download interception and native-messaging context forwarding.
- Added automated development and release browser-extension packaging, including stable Chrome CRX signing, unlisted Firefox XPI signing, application-package bundling, and Debian Chrome registration.
- Added a platform-aware capability and project-scope matrix.
- Added guarded automatic archive extraction with traversal and symbolic-link checks.
- Added Secret Service credential-vault integration without persisting vault-backed passwords in request JSON.
- Added NetworkManager metered-network policies to allow, hold, or pause and resume downloads.
- Added maintained social-site extraction through yt-dlp, excluding YouTube.
- Added explicit DRM-manifest detection and safe refusal; DRM bypass remains intentionally unsupported.
- Hardened Debian, AppImage, and Flatpak release packaging with clean source
  staging, deterministic artifacts, verified packaging tools, complete AppStream
  metadata, a supported Flatpak runtime, scoped sandbox permissions, and bundled
  Flatpak helper tools.

## 0.1.0

- Initial Linux-native Qt6 Widgets application.
- Added libcurl multi + Linux epoll download engine.
- Added segmented downloads, sparse mmap writes, SQLite persistence, scheduler, site grabber, ZIP/ZIP64 preview, import/export, native messaging browser integration, and Linux packaging.
