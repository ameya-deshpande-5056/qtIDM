# Changelog

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
