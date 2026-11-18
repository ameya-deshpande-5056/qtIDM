# qtIDM capability matrix

This document records the current implementation state and intentionally
out-of-scope functionality of the Linux desktop application. Browser-related
capabilities may be provided directly by qtIDM or through its Chrome and
Firefox integrations.

## Downloading

| Capability | qtIDM state |
| --- | --- |
| HTTP, HTTPS, FTP | Implemented |
| Multipart transfer, 4 KiB-aligned dynamic planning, stalled-connection preemption, up to 32 connections | Implemented |
| DNS-over-HTTPS (DoH) (`CURLOPT_DOH_URL`) | Implemented (Cloudflare, Google, Quad9, and custom resolvers) |
| Browser-like TLS & HTTP header camouflage | Implemented (Chrome and Firefox OpenSSL TLS 1.2/1.3 cipher lists, EC curves, and strict browser header ordering) |
| Pause, resume, cancel, retry, `Retry-After` | Implemented |
| Persistent resume after restart | Implemented, including request headers, authentication, proxy, DoH, `refreshTarget`, checksum, timeout, and retry options; transfers left queued, connecting, or downloading at exit are re-queued automatically on the next start |
| Per-download, per-host, and per-domain connection & throttling rules | Implemented (SQLite `domain_rules` supporting subdomains, `max_segments`, `max_concurrent_downloads`, speed limits, custom `User-Agent`, and custom headers) |
| Global/queue concurrent download limits | Implemented |
| Per-download and default speed limits | Implemented |
| Configurable retries, retry delay, connect timeout, low-speed timeout, redirects | Implemented |
| Basic/digest server authentication | Implemented through libcurl |
| HTTP and SOCKS proxies, including proxy credentials in the URL | Implemented through libcurl |
| Custom headers, cookies, referrer, User-Agent | Implemented; browser extensions forward browser context, page title, and origin page URL |
| Magic-byte format sniffing & semantic file naming | Implemented (64-byte magic header inspection combined with browser page title for generic/opaque filenames) |
| Duplicate URL detection | Implemented |
| Existing-file rename, overwrite, or skip | Implemented |
| Disk-space preflight and atomic `.part` publication | Implemented |
| MD5, SHA-1, SHA-256, SHA-512 validation | Implemented |
| Replace expired URL while preserving ranges | Implemented (both manual replacement and automatic site-adapter recovery for MediaFire, Google Drive, and GoFile) |
| Named queues, priorities, time/day windows, repeat schedules | Implemented |
| Post-download executable | Implemented without invoking a shell; `{file}`, `{dir}`, and `{url}` arguments are supported |
| Move file or remove list entry after successful download | Implemented |
| Import/export history | Implemented as JSON |
| Import/export newline-separated links | Implemented |
| Clipboard monitoring for single or multiple links | Implemented |
| Automatic file categories | Implemented |
| Search, category/status filters, column sorting | Implemented |
| Size, speed, ETA, progress, multipart visualization | Implemented |
| Multi-select start, pause, cancel, delete | Implemented |
| Open file/folder, copy URL/path, calculate checksum | Implemented |
| Completion/error desktop notifications | Implemented |
| Per-session data-use quota and reset | Implemented across concurrent HTTP/HTTPS/FTP transfers, including unknown-length responses |
| Metered-network rules | Implemented through NetworkManager D-Bus: allow, hold new downloads, or pause active downloads and resume when unmetered |
| Battery-level stop rule | Not implemented; currently outside the desktop policy scope |
| Automatic archive extraction | Implemented through 7-Zip for ZIP, 7z, RAR, and tar-family archives, with path-traversal and symbolic-link rejection |
| Secure credential vault and per-domain password reuse | Implemented through the desktop Secret Service using `secret-tool`; vault-backed passwords are excluded from persisted request JSON |
| Recursive same-origin site grabbing | Implemented, including optional JavaScript execution, rendered-DOM saving, and dynamic `href`, `src`, and `srcset` discovery through Chrome/Chromium |

## Media

| Capability | qtIDM state |
| --- | --- |
| HLS and MPEG-DASH | Native multi-threaded HLS/M3U8 chunk downloader (`HlsDownloader`) with local FFmpeg remuxing; MPEG-DASH and fallback HLS via FFmpeg + HTTP/2 loopback proxy |
| Separate adaptive audio/video download and mux | Implemented |
| Browser manifest/direct media/subtitle/blob capture | Implemented in Chrome and Firefox extensions (including `blob:` URLs, open/closed/Declarative Shadow DOM, React/Vue/Preact/Svelte/Angular component state sniffing, and cross-origin iframes) |
| ServiceWorker synthesized stream downloads | Implemented via `window.__qtidm_sw_puller` and `SwStreamSessionManager` streaming over Native Messaging into `SparseFileWriter` |
| Host and file-extension capture exclusions | Implemented in both browser extensions |
| Cookies, referrer, User-Agent forwarding | Implemented |
| AES-128 HLS described by the manifest | Implemented natively in `HlsDownloader` (OpenSSL AES-128-CBC) and supported via FFmpeg |
| DRM media | Detection and explicit refusal are implemented; circumvention remains intentionally unsupported |
| Site-specific social-network extractors | Implemented through the maintained `yt-dlp` provider for supported YouTube, Bilibili, Instagram, Facebook, X/Twitter, TikTok, Reddit, Vimeo, Dailymotion, Twitch, and SoundCloud links |
| Built-in media player/stream action | Delegated to the desktop browser/media player |

## Browser and privacy

qtIDM deliberately integrates with installed Chrome/Firefox instead of
embedding another browser engine. Those browsers provide tabs, profiles,
incognito windows, bookmarks, history, password storage, page inspection,
site permissions, DNS controls, reader/display options, and mature content
blocking extensions. qtIDM adds download interception, context-menu capture,
cookie/referrer/User-Agent forwarding, media/resource capture, and manual
single/batch submission. Chrome and Firefox download interception is exercised
end-to-end in CI through each real browser and the native-messaging protocol.
Embedding and maintaining a second browser engine is intentionally outside the
application scope.

## Platform integration

Linux integration includes persistent queues, a system tray, desktop
notifications, D-Bus single-instance forwarding, browser native messaging,
NetworkManager metered-state detection, Secret Service credentials, and
desktop autostart/service management. Tagged builds bundle a private-key-signed
Chrome CRX and an unlisted Mozilla-signed Firefox XPI; Debian releases also
register the local CRX with Google Chrome.

## Automation

| Capability | qtIDM state |
| --- | --- |
| Session D-Bus service (`io.qtidm.Qtidm.Application`) | Implemented; also carries single-instance URL forwarding |
| Download list as JSON (`ListDownloads`) | Implemented; id, file, url, path, status, size, downloaded, speed, added, category, queue, plus total speed, queues, and free space |
| Pause (`PauseDownloads`) | Implemented for active transfers and media jobs |
| Start/resume (`StartDownloads`) | Implemented; rebuilds the request, forces `Start now`, and refuses a duplicate queue entry |
| Remove (`RemoveDownloads`) and delete (`DeleteDownloads`) | Implemented; `delete` also removes the file and `.part` file |
| Per-id results | Implemented as `done` and `skipped` with a reason (`unknown id`, `not active`, `not resumable`, `already complete`, `already queued`, `media control failed`) |
| Queue dispatch control (`StartQueue`, `StopQueue`) | Implemented per named queue; `Main` is always addressable and always listed |
| Adaptive HLS/DASH and social-media jobs | Implemented through the window's media downloader, so pause, start, and delete behave like a normal transfer |
| Discovery file (`ipc.json`) | Implemented with a per-start token, owner-only permissions, and the session bus name it publishes (suffixed for a portable copy) |
| Alternate data directory (portable mode) | Implemented through `--data DIR` and `QTIDM_DATA_DIR`; settings, database, logs and `ipc.json` move together, and the instance takes its own D-Bus name so it can run beside an installed copy |
| `qtidm-cli` | Implemented: `list`, `pause`, `start`, `remove`, `delete`, `start-queue`, `stop-queue`, `--json`, `--data` |
| Adding downloads from `qtidm-cli` | Not implemented; use the D-Bus `AddUrl`, `AddUrls`, or `AddDownloadsJson` verbs |
| Loopback HTTP automation transport | Not implemented; `ipc.json` reserves `port` for it |
| MCP server for AI assistants | Not implemented |

## Scope and remaining work

- BitTorrent support is outside the project scope.
- DRM circumvention is outside the project scope.
