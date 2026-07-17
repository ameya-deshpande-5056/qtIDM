# qtIDM Test Plan

## Automated CTest Coverage

- `qtIDM_core_tests`
  - Create SQLite schema.
  - Store/list downloads.
  - Persist segments.
  - Update progress.
  - Update status.
  - Delete download and segment rows.

- `qtIDM_download_types_tests`
  - Round-trip every `DownloadStatus`.
  - Serialize/deserialize full `DownloadRequest`.
  - Serialize/deserialize `DownloadRecord` with segments.

- `qtIDM_import_export_tests`
  - Export history JSON.
  - Import history JSON.
  - Preserve segment state.
  - Reject invalid import JSON.

- `qtIDM_sparse_file_writer_tests`
  - Create sparse target file.
  - Write data at multiple offsets.
  - Verify file size.
  - Reject out-of-range writes.
  - Reject writes before open.

- `qtIDM_site_grabber_tests`
  - Crawl same-origin file links.
  - Respect crawl depth.
  - Create `DownloadRequest` entries.
  - Report fetch errors.

- `qtIDM_scheduler_tests`
  - Persist future queue.
  - Reload queued requests.
  - Preserve URL and segment count.
  - Persist named-queue concurrency and enabled state.
  - Prevent paused queues from dispatching and resume them on enable.
  - Assign stable schedule IDs and edit, reorder, and remove pending entries.

- `qtIDM_packaging_metadata_tests`
  - Validate `.desktop` launch metadata.
  - Validate Chrome native messaging manifest JSON.
  - Validate Firefox native messaging manifest JSON.
  - Validate browser extension manifest JSON.

- `qtIDM_zip_preview_tests`
  - Parse classic ZIP central directory.
  - Parse ZIP64 entry sizes.
  - Parse ZIP64 EOCD count and offset.
  - Parse 70,000-entry ZIP64 archive.
  - Reject missing EOCD.
  - Reject missing ZIP64 locator.
  - Reject invalid central directory offset.

- `qtIDM_downloader_integration_tests`
  - Start local Python HTTP fixture server.
  - Complete segmented range download.
  - Verify downloaded bytes.
  - Fall back on non-range server.
  - Complete basic-auth download.
  - Reject resume when remote size changes.
  - Pause active transfer.
  - Cancel active transfer.

- `qtIDM_media_downloader_tests`
  - Recognize HLS and MPEG-DASH manifest URLs.
  - Download and mux separate DASH audio/video tracks with FFmpeg.

- `qtIDM_performance_smoke_tests`
  - Seed 10,000 persisted downloads.
  - Verify list/load path remains under budget.

Run:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Manual / Integration Cases

- Main window opens and restores persisted history.
- Add URL dialog accepts URL, path, category, segments, proxy, auth, speed limit, and schedule time.
- Start resumes selected persisted download.
- Pause preserves segment offsets.
- Stop cancels active segment transfers.
- Delete removes selected history row and persisted record.
- Segment grid updates while downloading.
- Tray menu opens app, adds URL, and quits.
- System theme change updates light/dark stylesheet through portal D-Bus.
- Single-instance D-Bus forwards URL to existing process.
- Native messaging host receives URL and headers from browser extension.
- Chrome extension context-menu link capture sends URL, cookie, referrer, and user-agent.
- Firefox extension context-menu link capture sends URL, cookie, referrer, and user-agent.
- HTTPS download completes.
- FTP download completes.
- Speed limit caps transfer rate.
- Proxy URL routes transfer.
- Username/password authentication is applied.
- Site grabber queues same-host links.
- ZIP preview shows classic ZIP entries.
- ZIP preview shows ZIP64 entries over 4 GiB.
- Import restores exported history.
- Scheduler dispatches due queued download.
- Scheduler edits, reorders, and removes pending entries.
- Scheduler persists per-queue concurrency and pause/resume state.
- `.deb` installs, launches, and removes cleanly.
- AppImage runs on a clean supported distribution.
- Flatpak bundle installs, launches, and has network/session-bus access.
- GitHub tag `vX.Y.Z` matching `CMakeLists.txt` publishes release assets.
- GitHub mismatched tag/version fails release workflow.

## GitHub Actions Flow

The single workflow `.github/workflows/build-release.yml` runs sequentially:

1. Install Linux dependencies.
2. Configure.
3. Build.
4. Run all CTest tests.
5. Run scheduled performance smoke on weekly schedule.
6. Prepare release metadata for tag/manual release.
7. Build Debian package.
8. Install and smoke-test Debian package.
9. Build AppImage.
10. Smoke-test AppImage.
11. Build Flatpak bundle.
12. Install and smoke-test Flatpak bundle.
13. Upload artifacts.
14. Publish GitHub Release for version tags.
