# Changelog

## Unreleased

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

## 0.1.0

- Initial Linux-native Qt6 Widgets application.
- Added libcurl multi + Linux epoll download engine.
- Added segmented downloads, sparse mmap writes, SQLite persistence, scheduler, site grabber, ZIP/ZIP64 preview, import/export, native messaging browser integration, and Linux packaging.
