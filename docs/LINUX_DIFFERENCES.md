# Linux Differences

This project does not ship copied IDM trademarks, icons, binary assets, fonts, or proprietary UI resources.

Linux-specific behavior:

- Single instance uses the session D-Bus instead of Windows mutex/window messages.
- Theme tracking uses `org.freedesktop.portal.Settings`.
- Browser capture uses native messaging hosts for Chrome and Firefox.
- Desktop integration uses `.desktop`, shared MIME info, and Linux packaging metadata.

Implemented original Linux-native functionality:

- Segmented HTTP/HTTPS/FTP transfers through libcurl multi and Linux `epoll`.
- Up to 32 segments per download.
- Sparse file allocation with mmap-backed window writes and `pwrite` fallback.
- Persisted SQLite download, segment, queue, and history state.
- Crash recovery through persisted segment offsets and selected-download resume.
- Scheduler UI and persistent scheduled queue.
- Speed limit, proxy, and username/password authentication fields.
- Same-origin site grabber with optional Chrome/Chromium JavaScript rendering.
- ZIP and ZIP64 central-directory preview.
- Automatic archive extraction through 7-Zip with unsafe-entry rejection.
- Secret Service credential storage through `secret-tool`.
- NetworkManager metered-network policies for holding or pausing transfers.
- Social-site media extraction through `yt-dlp`, excluding YouTube.
- Explicit DRM detection and refusal without circumvention.
- JSON import/export.
- Chrome and Firefox native messaging extension source.
- Release-signed CRX and unlisted Mozilla-signed XPI artifacts bundled with application packages.
- Debian registration of the bundled CRX as an external Google Chrome extension.
- Real-browser Chrome and Firefox native-messaging interception tests.
- Pause, stop, delete, and selected-download resume actions.
- AppImage, Flatpak, and Debian package build scripts.

Known limitations:

- Chrome release builds require a persistent signing key and matching `QTIDM_CHROME_EXTENSION_ID`; Firefox uses the unlisted AMO signing channel with ID `qtidm@io.github.qtidm`.
- Flatpak builds do not install host browser native messaging manifests; use a host package for browser integration.
- This is an original IDM-like Linux application, not a proprietary IDM clone.
