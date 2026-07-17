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
- Site grabber for same-host links.
- ZIP and ZIP64 central-directory preview.
- JSON import/export.
- Chrome and Firefox native messaging extension source.
- Pause, stop, delete, and selected-download resume actions.
- AppImage, Flatpak, and Debian package build scripts.

Known limitations:

- Browser extension IDs must be filled in after local signing/installation.
- This is an original IDM-like Linux application, not a proprietary IDM clone.
