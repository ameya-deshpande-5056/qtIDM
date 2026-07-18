# qtIDM capability matrix

This document records the current implementation state and intentionally
out-of-scope functionality of the Linux desktop application. Browser-related
capabilities may be provided directly by qtIDM or through its Chrome and
Firefox integrations.

## Downloading

| Capability | qtIDM state |
| --- | --- |
| HTTP, HTTPS, FTP | Implemented |
| Multipart transfer, dynamic part refill, up to 32 connections | Implemented |
| Pause, resume, cancel, retry, `Retry-After` | Implemented |
| Persistent resume after restart | Implemented, including request headers, authentication, proxy, checksum, timeout, and retry options |
| Per-download and per-host connection limits | Implemented |
| Global/queue concurrent download limits | Implemented |
| Per-download and default speed limits | Implemented |
| Configurable retries, retry delay, connect timeout, low-speed timeout, redirects | Implemented |
| Basic/digest server authentication | Implemented through libcurl |
| HTTP and SOCKS proxies, including proxy credentials in the URL | Implemented through libcurl |
| Custom headers, cookies, referrer, User-Agent | Implemented; browser extensions forward browser context |
| Duplicate URL detection | Implemented |
| Existing-file rename, overwrite, or skip | Implemented |
| Disk-space preflight and atomic `.part` publication | Implemented |
| MD5, SHA-1, SHA-256, SHA-512 validation | Implemented |
| Replace expired URL while preserving ranges | Implemented |
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
| HLS and MPEG-DASH | Implemented through FFmpeg |
| Separate adaptive audio/video download and mux | Implemented |
| Browser manifest/direct media/subtitle capture | Implemented in Chrome and Firefox extensions |
| Host and file-extension capture exclusions | Implemented in both browser extensions |
| Cookies, referrer, User-Agent forwarding | Implemented |
| AES-128 HLS described by the manifest | Delegated to FFmpeg |
| DRM media | Detection and explicit refusal are implemented; circumvention remains intentionally unsupported |
| Site-specific social-network extractors | Implemented through the maintained `yt-dlp` provider for supported Instagram, Facebook, X/Twitter, TikTok, Reddit, Vimeo, Dailymotion, Twitch, and SoundCloud links; YouTube is intentionally excluded |
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

## Scope and remaining work

- BitTorrent support is outside the project scope.
- DRM circumvention is outside the project scope.
