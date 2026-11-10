# Changelog

## 0.3.0

- Overhauled the main download table in `MainWindow` to display `File Name` (with MIME-type file icons via `QMimeDatabase`), `Size`, `Status`, `Progress`, `Download Speed`, `Date Added`, `ETA`, and `Save To`, removing the redundant `Source URL` and inline `Segments` columns while keeping source URLs searchable, copyable, and exportable via `Qt::UserRole` and retaining segment inspection in the bottom `Segments` tab.
- Added `SortableTableItem` so `Size`, `Progress`, `Download Speed`, `Date Added`, and `ETA` sort by their underlying numeric values or timestamps rather than formatted text.
- Added viewport-responsive column sizing (`adjustDownloadColumnsForViewport`) to proportionally expand and shrink `File Name` and `Save To` when the window resizes while preserving interactive column resizing.
- Added `ElidedTooltipDelegate` with single-line right-elided cells across the download table, category tree, and details/segment views so full values appear in tooltips only when text is elided, paired with explicit `QToolTip` styling for dark and light themes in `ThemeManager::styleSheet`.
- Synchronized status and metric updates across the download table and `General` details pane (`lastStatuses_`, `applyRowStatusUi`), keeping `Connecting`, `Downloading`, retry, and terminal states consistent, preserving row selection on updates, and restoring the configured `record.request.segments` count when resuming downloads.
- Connected single and double clicks on the system tray icon (`QSystemTrayIcon::activated`) to restore hidden or minimized windows while preserving maximized state, set the `"qtIDM"` tray tooltip, and added `StartupWMClass=qtIDM` to `assets/io.qtidm.Qtidm.desktop`.
- Offloaded DNS validation, IP pinning, and `probeSize` metadata probing in `CurlEpollDownloader` to a pool of 4 background worker threads (`runProbeWorker`), keeping the epoll transfer loop responsive and supporting immediate probe cancellation (`abortCallback` and `deferredControls_`) on cancel, pause, or shutdown.
- Updated `CurlEpollDownloader` segment and retry scheduling (`refillQueuedSegments`, `addDueRetries`, `batchOrder_`) to fairly balance connections across concurrent batches by fewest active connections while enforcing both per-download and per-host connection limits, including dynamic segment splitting (`splitActiveSegment`).
- Improved libcurl + epoll event handling in `CurlEpollDownloader` by disabling `CURLOPT_PIPEWAIT` for multi-segment transfers so parallel TCP connections are opened instead of serializing behind HTTP/2, capping `CURLMOPT_MAX_CONCURRENT_STREAMS` at 2, tracking `curlTimeoutDeadlineMs_` accurately in `timerCallback`, handling `EINTR` and `EPOLLHUP`, removing edge-triggered `EPOLLET`, and separating periodic 250 ms `emitBatchProgress` updates.
- Unified error cleanup in `CurlEpollDownloader::failBatch` to cancel sibling transfers and pending retries, persist segment checkpoints, and emit a single terminal `Failed` status; added `reportedStatus` tracking across `Connecting` and `Downloading` transitions, HTTP/1.1 fallback on `CURLE_OPERATION_TIMEDOUT` retries, and a guard preventing 4xx/5xx errors from being misreported as byte-range failures.
- Improved context-menu batch link collection (`sendBatchToHost` and `qtidm-selected-links`) in `browser/chrome/service_worker.js` and `browser/firefox/background.js` to walk non-collapsed selection ranges, support SVG `href.baseVal` and enclosing anchor elements, validate `http`/`https`/`ftp` schemes, deduplicate up to 100 links, forward frame `Referer` URLs, guard negative `frameId` values, and report a clear error when no downloadable links are found.
- Refreshed `README.md` with a centered logo and summary, build/release/technology/license badges, navigation links, clean-room standards note, and an application and browser-extension screenshot gallery under `assets/screenshots/`.
- Added the `qtIDM_main_window_tests` suite (`tests/MainWindowTest.cpp`, running headless with `QT_QPA_PLATFORM=offscreen`) covering status synchronization, tray window restoration, column layout, dynamic resizing, and elided-cell tooltips.
- Expanded `DownloaderIntegrationTest` (`concurrentBatchSharesHostLimitAndReportsHeaderTimeoutAccurately` and `/stall-headers.bin` fixture) and `tests/browser_interception_test.cjs` to cover retry status sequences, shared per-host limits, header timeouts, frame-aware selected-link batching, and empty-selection rejection.

## 0.2.9

- Added URL path normalization in `SocialMediaExtractor::normalizeUrl` for international Bilibili URLs, automatically stripping the `/en` path prefix (`/en/video/` -> `/video/`) so `yt-dlp` matches its dedicated Bilibili extractor rather than falling back to the generic extractor (`ext: html`) which returned no downloadable video or audio streams.
- Updated browser extension popup logic (`browser/chrome/popup.js` and `browser/firefox/popup.js`) with `normalizeMediaUrl` to sanitize international Bilibili URLs before sending extraction requests to qtIDM.
- Added comprehensive unit test coverage in `SocialMediaExtractorTest` verifying `normalizeUrl` transforms `/en/video/...` paths while preserving standard Bilibili URLs and unrelated domains.

## 0.2.8

- Replaced hardcoded fallback extensions in `resolveDownloadFilename` and browser extension background scripts with a comprehensive MIME-to-extension dictionary (`extensionFromMime`), supporting audio, video, subtitle, document, and archive formats (Opus, WebM, FLAC, AAC, M4A, MP4, MP3, MKV, MOV, VTT, SRT).
- Implemented container validation and mismatch correction in filename resolution across single and batch download dialogs, correcting mismatched title extensions (such as page titles ending in `.mp4` for Opus or WebM streams) according to authoritative `Content-Type` headers (`_qtidmMimeType`) or URL paths to prevent file extension mismatch and unplayable files.
- Enhanced browser extension media capture resolution (`mediaExtension` and `mediaSuggestedFilename`) so direct audio and video streams lacking URL path extensions dynamically adopt their proper container extension from their response `Content-Type`.
- Forwarded intercepted `Content-Type` from browser extensions to qtIDM as `_qtidmMimeType` to ensure consistent format detection between the browser and desktop application.
- Fixed a layout issue in the browser extension popup where the `#notice` status footer floated upward into the middle of the popup when only one or few media items were captured; converted the popup container to a flex column layout with `margin-top: auto; flex-shrink: 0;` so the footer is permanently anchored to the bottom of the popup.
- Made the extension popup active panels internally scrollable (`overflow-y: auto`) within bounded height (`min-height: 420px; max-height: 600px;`), keeping the header, navigation tabs, and status footer pinned in place while long capture lists or settings scroll smoothly.
- Added visual styling enhancements to the extension popup, including distinctive accent borders and type indicators for adaptive streams (`.item.stream`), full-height empty state placeholders, and descriptive stream title formatting based on tab titles.
- Enhanced `yt-dlp` discovery in `SocialMediaExtractor` to query and compare versions across all available installation sources (user pip in `~/.local/bin`, pyenv in `~/.pyenv/shims`, Python 3 modules via `python3 -m yt_dlp`, `/usr/local/bin`, and system distribution packages in `/usr/bin`), dynamically prioritizing and running whichever source has the newest release.
- Updated documentation and CI configurations to install and recommend `yt-dlp` via pip / `pipx` for up-to-date extractor definitions rather than outdated distribution packages.
- Resolved a pipe buffering race condition in `DownloaderIntegrationTest` ALPN test cases where the local HTTP/2 mock server's CA certificate path was read before being fully flushed to stdout.
- Fixed AppImage packaging failure with continuous `appimagetool` by explicitly staging the application icon (`io.qtidm.Qtidm.png`), creating `.DirIcon`, and passing `--desktop-file` and `--icon-file` in `packaging/build-appimage.sh`.
- Bundled `7zz`/`7z` and `secret-tool` in AppImage packaging when present on the host to ensure self-contained archive extraction and credential retrieval.
- Updated Debian packaging recommendations to `pipx | yt-dlp` to ensure users install up-to-date PyPI extractor packages rather than stale distribution packages.
- Upgraded Flatpak packaging dependencies to the newest upstream releases (`yt-dlp` 2026.10.26, 7-Zip 26.05 from GitHub mirror `ip7z/7zip`, and `libsecret` 0.21.8).
- Expanded automated CI dependency updater (`packaging/release/update-linuxdeploy-hashes.sh`) to automatically monitor, verify, and bump Flatpak dependencies alongside continuous `linuxdeploy` AppImage hashes.
- Added comprehensive unit test coverage in `DownloadTypesTest` and `browser_interception_test.cjs` covering dynamic MIME mappings, extension overrides, and container correction.

## 0.2.7

- Enhanced `CurlEpollDownloader` DNS inspection and address pinning (`isPrivateOrLoopbackHost`) to discover, validate, and preserve all resolved public IPv4 and IPv6 addresses for multi-homed hosts and anycast CDN networks (such as GitHub release assets hosted on Fastly CDN), eliminating single-point-of-failure hangs when individual CDN edge nodes time out or drop packets.
- Standardized all pinned IP addresses according to libcurl `CURLOPT_RESOLVE` syntax (including square-bracketed notation `[...]` for IPv6 addresses), enabling libcurl's built-in multi-IP connection failover across all validated CDN addresses during the initial request probe and segment transfers.
- Added automatic address rotation in `CurlEpollDownloader::retryTransfer`, cycling the pinned IP list so subsequent retry attempts immediately target the next healthy CDN edge node without waiting through redundant connection timeouts on unresponsive nodes.
- Added test coverage in `DownloaderIntegrationTest` verifying multi-IP discovery and validation for multi-homed domains while strictly preserving private, loopback, and cloud metadata SSRF boundaries.
- Resolved an E2E test assertion defect in `tests/browser_extension_e2e.py` where repeated fixture download triggers in Firefox resulted in queued prepare requests, verifying persistent port reuse across all pre-download messages rather than assuming an exact two-message sequence.

## 0.2.6

- Added automated fallback handling in `CurlEpollDownloader` for server-side byte-range compression corruption (such as Cloudflare returning compressed lengths in `Content-Range` while streaming uncompressed payloads, causing `response exceeds its declared Content-Range`). The engine automatically cancels split transfers, truncates staging, reconfigures the transfer to a single connection without dynamic range splitting, and seamlessly restarts as an unsegmented full-file `GET` request.
- Implemented an ephemeral loopback HTTP/2 stream proxy (`MediaStreamProxy`) that negotiates TLS ALPN (`h2`) with libcurl and forwards browser headers (`Referer`, `Origin`, `User-Agent`, `Cookie`), enabling native downloads of HLS/DASH streams hosted on HTTP/2-only CDNs and Cloudflare-protected endpoints (such as `vault-14.owocdn.top` / `uwu.m3u8`) that reject FFmpeg's HTTP/1.1 client with 403 Forbidden.
- Added virtual playlist rewriting in `MediaStreamProxy` to normalize disguised media extensions (e.g. `.jpg` segment files) to virtual `.ts` paths, preventing FFmpeg from misdetecting MPEG-TS streams as image formats.
- Added combined video and audio stream extraction and remuxing in `SocialMediaExtractor` and `MediaDownloader` for DASH and adaptive streaming platforms (Bilibili, YouTube) that serve video and audio as separate tracks. Each video format is automatically paired with the highest-bitrate compatible audio track (matching MP4 with AAC/M4A, WebM with Opus), presented at the top of the format dialog sorted by resolution, and downloaded simultaneously into a unified container (`.mp4`, `.webm`) using FFmpeg with lossless copy (`-c copy`).
- Enhanced `SocialMediaExtractor` executable discovery to check user-local installation directories (`~/.local/bin/yt-dlp` and `~/.pyenv/shims/yt-dlp`) in addition to system `PATH` so qtIDM reliably locates the latest updated `yt-dlp` release even when launched from desktop menus.
- Added smart URL routing and popup awareness for YouTube and Bilibili, providing direct "Download with qtIDM (yt-dlp)" prompts in the browser extension popup and automatically offering Social Media stream extraction when adding media URLs via the standard "Add URL" dialog or context menu.

## 0.2.5

- Fixed an issue where adaptive stream (`.m3u8`, `.mpd`) suggested filenames were truncated at periods/full-stops (`.`) in titles (such as `Steel Ball Run: JoJo's Bizarre Adventure Ep. 7` becoming `Ep.mkv`) by replacing `QFileInfo::completeBaseName()` with extension-targeted stripping (`MediaDownloader::suggestedFilename`), fully preserving periods in title numbers, abbreviations, and names across single and batch download dialogs.
- Improved media title filename sanitization across browser extensions and native application dialogs, converting unsupported filesystem characters (`/`, `\`, `:`, `*`, `?`, `"`, `<`, `>`, `|`) to safe readable equivalents (` - `, `'`, `_`) while preserving full Unicode titles and preventing truncation at path or title separators.
- Corrected file collision deduplication in the downloader reactor, download scheduler, and batch dialogs to extract suffixes using the final dot rather than the first period, preventing title corruption for files containing periods.
- Unblocked YouTube and Bilibili in site-specific social media extraction, removing the hardcoded prohibition in the Social Media dialog and adding `youtube.com`, `youtu.be`, `bilibili.com`, `b23.tv`, and `bili2233.cn` to supported extraction hosts.
- Fixed a CSS specificity defect in the browser extension popup where `.empty { display: flex; }` overrode the HTML `hidden` attribute, causing the "No downloadable media yet" empty placeholder box to render simultaneously underneath captured media cards.
- Added `"all_frames": true` to Chrome and Firefox content script definitions so streaming media is detected inside embedded player iframes (e.g. Kwik, RapidCloud, MegaCloud).
- Added direct DOM media element observers (`play`, `playing`, `loadedmetadata`, `canplay` and a `MutationObserver` on `<video>`, `<audio>`, and `<source>` tags) in the content bridge to reliably capture media streams even when site Content Security Policy (CSP) blocks inline script injection.
- Added decoupled request and Service Worker tab resolution (`resolveTabId`) in the extension service worker and background script, correlating non-tab media buffering requests (`tabId: -1`) to active tabs matching the request's initiator origin.
- Extended direct media detection (`isDirectMediaResponse`) to recognize video and audio content returned from `fetch()` and `XMLHttpRequest` requests while ignoring transport stream segments (`.ts`, `.m4s`, `.key`, `.vtt`).
- Added real-time captures list refresh in the extension popup using storage change listeners and runtime broadcast events (`mediaCaptured`), updating the panel dynamically as playback begins without requiring the user to close and reopen the panel.
- Filtered out internal metadata headers starting with `_` (such as `_qtidmMediaType`) from being forwarded in outbound HTTP requests by FFmpeg (`MediaDownloader`) and `CurlEpollDownloader`, preventing CDN and Cloudflare WAF 403 Forbidden errors.

## 0.2.4

- Hardened DNS rebinding and TOCTOU protection by pinning validated public IP addresses via `CURLOPT_RESOLVE`, rejecting untrusted JSON `pinnedIp` injection, and enforcing fail-closed handling for private, loopback, or unresolvable domains.
- Verified single-connection HTTP/2 multiplexing across concurrent streams over TLS ALPN (`h2`), adding trusted local CA certificate configuration alongside optional self-signed test overrides.
- Implemented strict RFC 6265 cookie scoping, enforcing host-only versus domain cookie separation, hierarchical path filtering, plaintext HTTP secure cookie suppression, and expired cookie rejection.
- Updated continuous release CI workflows with refreshed `linuxdeploy` continuous SHA-256 checksums, and raised the Firefox extension `strict_min_version` to 142.0 for AMO compliance with `data_collection_permissions`.
- Hardened engine durability by switching SQLite to WAL mode with `synchronous=FULL`, enforcing durable checkpoint flushes before persisting segment layout changes, and verifying byte-exact recovery from out-of-process `SIGKILL` termination.

## 0.2.3

- Improved protected HLS/DASH downloads by restoring a missing browser `Origin` header and giving FFmpeg additional time and data to detect sparse codec metadata.
- Replaced raw FFmpeg failures for expired or denied media sessions and image/preview manifests with concise recovery guidance.
- Updated the release workflow to Node.js 24-compatible major versions of the checkout, Node setup, artifact upload, and GitHub release actions.
- Fixed unknown-length downloads growing beyond their real size after a connection drop by resuming with validated open-ended HTTP Range requests instead of appending a restarted response.
- Improved remote-size detection by falling back from HEAD to a one-byte Range probe, reading totals from `Content-Range`, and adopting metadata from the actual download response.
- Hardened resumed downloads with exact segment and final-file size validation, per-range write bounds, preserved unknown-length partial files, and persisted ETag/Last-Modified source checks.
- Added regression coverage for missing or rejected HEAD metadata, unknown-length automatic and manual resume, ignored Range requests, changed remote entities, and committed-byte session accounting.

## 0.2.2

- Changed new download queues to allow unlimited concurrent downloads by default, while retaining optional per-queue limits from 1 to 64 and preserving existing saved limits.
- Redesigned the alternate speed-limit control with larger, centered speedometer icons and distinct needle directions for inactive and active states.
- Reworked the About dialog into a balanced two-column layout with the branding icon on the left and application details on the right.

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
