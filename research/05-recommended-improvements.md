# 05 - Recommended Technical Improvements for qtIDM

**Document Status:** Engineering Proposal & Specification  
**Project:** qtIDM  
**Date:** 2026-10-30  

---

## 1. Overview of Proposed Enhancements

The reverse-engineering of Internet Download Manager (`IDMan.exe` and CRX `6.43.1`) revealed specific architectural mechanisms that explain its performance and reliability advantages in production. Based on these verified findings, we propose four targeted, high-impact improvements to qtIDM that retain its clean C++20/Qt6 architecture and Linux-native advantages while adopting IDM's proven algorithmic strengths.

---

## 2. Improvement 1: Dynamic Active-Segment Re-Splitting (Throughput & Scheduling)

### Affected Files
- [`src/core/CurlEpollDownloader.h`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.h)
- [`src/core/CurlEpollDownloader.cpp`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.cpp)
- [`tests/DownloaderIntegrationTest.cpp`](file:///home/ameya/qtIDM/tests/DownloaderIntegrationTest.cpp)

### Current Implementation
qtIDM pre-calculates dynamic parts at initiation:
$$\text{dynamicParts} = \min\left(\text{segments} \times 4, \max\left(1, \frac{\text{total} + 65536 - 1}{65536}\right)\right)$$
These parts are enqueued at download startup. When a segment transfer completes, `refillQueuedSegments()` checks for remaining idle queued segments. Once all initial parts have been dispatched, active connections cannot be split. If one connection stalls or lags on the final 20% of its range while all other connections complete, the download throughput drops to a single stream ("tail starvation").

### Proposed Change
Adopt IDM's `FindMaxFreeChunk` algorithmic pattern:
1. When any segment finishes downloading and the active connection count is below the configured `connectionLimit` (and `perHostConnectionLimit`):
2. Inspect all active, in-flight segments in the batch.
3. Calculate the remaining unread byte distance for each active transfer:
   $$\text{unread} = \text{segment.end} - (\text{segment.start} + \text{segment.written})$$
4. Find the segment with the largest $\text{unread}$ byte distance.
5. If $\text{unread} \ge \text{DYNAMIC\_SPLIT\_THRESHOLD}$ (recommend 256 KiB on local/fast links, minimum 64 KiB):
   - Calculate midpoint: $\text{mid} = \text{segment.start} + \text{segment.written} + \lfloor\text{unread} / 2\rfloor$.
   - Truncate the running segment's `end` boundary to $\text{mid}$.
   - Create a new `SegmentInfo` representing range $[\text{mid} + 1, \text{originalEnd}]$ with `status = DownloadStatus::Queued`.
   - Add the new segment to `batch->segments`, update the SQLite database via `DownloadRepository::updateSegments()`, and call `startQueuedSegments()` to immediately spawn a new connection on that sub-range.
   - Signal the running curl transfer of the truncated segment (or let it conclude cleanly when its received byte count reaches the updated truncated length).

### Justification
Eliminates tail latency completely. Downloads maintain 100% connection pool saturation from start to finish, matching IDM's behavior on throttled or rate-limited servers.

### Possible Regressions & Mitigations
- *Regression:* Sub-ranges become smaller than optimal TCP window ramp-up size.
  *Mitigation:* Enforce a minimum split threshold (e.g., do not split ranges smaller than 256 KiB).
- *Regression:* Server rejecting high connection concurrency.
  *Mitigation:* Obey existing `perHostConnectionLimit` and `batch->connectionLimit`.

### Automated Test Plan
Add test case `DynamicSegmentSplittingTest` in `DownloaderIntegrationTest.cpp`:
- Set up a test server returning a 10 MB payload where 1 range is deliberately throttled to 50 KB/s while other ranges run unthrottled.
- Verify that when the fast ranges complete, the slow range is automatically split, spawning a new unthrottled sub-range that finishes the transfer rapidly.

---

## 3. Improvement 2: Pre-Redirect Cookie & Authentication Jar Forwarding (Compatibility)

### Affected Files
- [`browser/chrome/service_worker.js`](file:///home/ameya/qtIDM/browser/chrome/service_worker.js)
- [`browser/firefox/background.js`](file:///home/ameya/qtIDM/browser/firefox/background.js)
- [`src/core/CurlEpollDownloader.cpp`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.cpp)
- [`tests/browser_interception_test.cjs`](file:///home/ameya/qtIDM/tests/browser_interception_test.cjs)

### Current Implementation
In `makeDownloadEntry()`, qtIDM queries cookies only for the target URL:
```javascript
const cookies = await chrome.cookies.getAll({ url }).catch(() => []);
```
When downloads are protected behind an OAuth/SSO gateway that redirects across domains (e.g. `auth.example.com` $\to$ `cdn.example.net`), session cookies bound to `auth.example.com` are lost.

### Proposed Change
Adopt IDM's multi-origin cookie capture:
1. In `redirectBrowserDownload()` / `makeDownloadEntry()`, compare `item.url` with `item.finalUrl`.
2. If `item.url !== item.finalUrl`:
   ```javascript
   const [targetCookies, initialCookies] = await Promise.all([
     chrome.cookies.getAll({ url: item.finalUrl }),
     chrome.cookies.getAll({ url: item.url })
   ]);
   ```
3. Merge the cookie sets into a unified `Cookie:` header, or pass an `initialCookies` header map to qtIDM.
4. In `CurlEpollDownloader::applyRequestOptions()`, forward all relevant cookies to libcurl.

### Justification
Guarantees reliable interception on authenticated file hosts, cloud storage providers (Google Drive, OneDrive, Mega, media hosts) that require gateway session cookies across redirects.

### Possible Regressions & Mitigations
- *Regression:* Leaking cookies to third-party domains on external redirects.
  *Mitigation:* Forward `initialCookies` only if the destination host shares the base domain or if the redirect chain explicitly originated from the authenticated site.

### Automated Test Plan
Add test in `browser_interception_test.cjs`:
- Simulate a redirect from `localhost:portA/download` (setting cookie `session=123`) to `localhost:portB/content`.
- Assert that the intercepted request object received by the native messaging host contains the authentication cookie from `portA`.

---

## 4. Improvement 3: HTTP/2 Multiplexing Tuning (Resource Optimization)

### Affected Files
- [`src/core/CurlEpollDownloader.cpp`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.cpp)
- [`src/core/CurlEpollDownloader.h`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.h)

### Current Implementation
qtIDM initializes `multi_ = curl_multi_init()` without setting pipelining/multiplexing options. Each easy handle initiates an independent TCP connection.

### Proposed Change
Enable libcurl's HTTP/2 multiplexing engine:
1. In `CurlEpollDownloader::start()`:
   ```cpp
   curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
   ```
2. In `CurlEpollDownloader::applyRequestOptions()`:
   ```cpp
   curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
   curl_easy_setopt(easy, CURLOPT_PIPEWAIT, 1L);
   ```
3. Libcurl will automatically negotiate ALPN `h2` and multiplex multiple Range requests over a single TLS/TCP connection when the remote server supports HTTP/2.

### Justification
Reduces connection latency, eliminates multiple TLS handshakes, and saves socket file descriptors while matching IDM's internal HTTP/2 multiplexing capability.

### Possible Regressions & Mitigations
- *Regression:* Servers with buggy HTTP/2 implementations closing streams on concurrent range requests.
  *Mitigation:* Fall back to HTTP/1.1 if an HTTP/2 protocol error (`CURLE_HTTP2`) occurs on retry.

### Automated Test Plan
Add test in `DownloaderIntegrationTest.cpp` connecting to an HTTP/2 server endpoint, asserting that multiple range requests complete successfully over a single underlying TCP connection.

---

## 5. Improvement 4: In-Page Streaming Media Detection Bridge (Media Capture)

### Affected Files
- [`browser/chrome/manifest.json`](file:///home/ameya/qtIDM/browser/chrome/manifest.json)
- [`browser/chrome/content_bridge.js`](file:///home/ameya/qtIDM/browser/chrome/content_bridge.js)
- [`browser/firefox/manifest.json`](file:///home/ameya/qtIDM/browser/firefox/manifest.json)
- [`browser/firefox/content_bridge.js`](file:///home/ameya/qtIDM/browser/firefox/content_bridge.js)

### Current Implementation
qtIDM captures media exclusively via the `webRequest` API by looking for `.m3u8`, `.mpd`, and media MIME types. Media fetched via modern MediaSource Extensions (MSE) where player scripts fetch manifests dynamically and load them into `blob:http://` URLs bypass `webRequest` detection.

### Proposed Change
Adopt IDM's lightweight in-page hook pattern:
1. In `content_bridge.js`, inject an in-page hook (`page_hook.js`) into the DOM before scripts execute (`document_start`).
2. Wrap `window.fetch` and `XMLHttpRequest.prototype.open` to inspect outgoing request URLs.
3. When a request matches an HLS (`.m3u8`), MPEG-DASH (`.mpd`), or video stream signature, send a `window.postMessage` to `content_bridge.js`, which relays the discovered manifest and its authorization headers to the background service worker.

### Justification
Significantly increases media capture coverage on modern single-page streaming websites without resorting to intrusive DRM circumvention.

### Possible Regressions & Mitigations
- *Regression:* Page script breakage if prototype wrappers are detected or fail.
  *Mitigation:* Use transparent proxy wrappers (`Reflect.apply`) and catch/swallow any hook errors silently.

### Automated Test Plan
Add end-to-end media test with a mock HTML5 player that dynamically fetches an `.m3u8` playlist via `fetch()` and verifies that qtIDM's extension badge increments and displays the captured stream.

