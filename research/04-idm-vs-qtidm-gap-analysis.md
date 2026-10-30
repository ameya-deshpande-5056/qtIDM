# 04 - IDM vs. qtIDM Technical Gap Analysis

**Document Status:** Complete Gap Analysis  
**Author:** Antigravity Engineering Research  
**Date:** 2026-10-30  
**Target Projects:** Internet Download Manager 6.43 (`IDMan.exe` / CRX `6.43.1`) vs. qtIDM 0.2.3 (`qtIDM` / WebExtensions `0.4.0`)  

---

## 1. Capability Classification Summary

| IDM Capability | Classification | Summary Comparison |
|---|---|---|
| **Multi-connection HTTP/HTTPS/FTP** | **Already implemented** | Both engines support parallel segmented Range downloads (up to 32 connections). |
| **Sparse Disk Writing** | **Superior in qtIDM** | qtIDM writes directly into one sparse `.part` file via `mmap`. IDM writes separate files per connection and concatenates them at completion ("Assembling"). |
| **Dynamic Segment Allocation** | **Partially implemented** | qtIDM partitions the download into smaller parts and refills connections on part completion. IDM dynamically re-splits the slowest active connection in real-time (`FindMaxFreeChunk`), eliminating tail latency. |
| **HTTP/2 Stream Multiplexing** | **Partially implemented** | IDM implements internal HTTP/2 stream multiplexing. qtIDM uses libcurl but does not enable `CURLMOPT_PIPELINING` / HTTP/2 multiplexing across ranges. |
| **Browser Download Interception** | **Already implemented** | Both implement two-stage browser download interception with seamless fallback to browser-native download on failure or rejection. |
| **Pre-Redirect Cookie Forwarding** | **Missing from qtIDM** | IDM captures cookies for both initial and target URLs across redirects (tags `[51]` and `[56]`). qtIDM only captures target URL cookies. |
| **In-Page API Hooking (`document.js`)** | **Missing from qtIDM** | IDM hooks `window.fetch` and `XMLHttpRequest` in the page context to capture streaming manifests from MSE / Blob URLs. qtIDM relies on `webRequest` API. |
| **Floating Video Download Button** | **Partially implemented** | IDM injects DOM buttons overlaying `<video>` elements. qtIDM lists captured streams in the extension popup menu. |
| **Persistence Engine** | **Superior in qtIDM** | qtIDM uses SQLite 3 in WAL mode with relational schemas. IDM stores state in the Windows Registry (`Software\DownloadManager`). |
| **Secret Service Credential Vault** | **Superior in qtIDM** | qtIDM integrates with FreeDesktop Secret Service via `secret-tool`. IDM uses Win32 Registry/DPAPI. |
| **Kernel Network Filter Drivers** | **Not applicable to Linux** | IDM bundles TDI and WFP drivers (`idmtdi.sys`, `idmwfp.sys`) for system-wide LSP/TDI interception. Linux utilizes browser extensions and standard sockets. |

---

## 2. Detailed Capability Comparisons

### 2.1 Dynamic Segment Allocation & Scheduling (High Priority)

#### 1. IDM Behaviour
When a connection completes or bandwidth becomes available, IDM does not leave the connection idle or rely only on pre-divided parts. It evaluates all in-progress connections, executes `ChunksList::FindMaxFreeChunk()` to locate the connection with the largest remaining byte range, and if the remaining range exceeds 23,363 bytes (`0x5b43`), splits the remaining range in half, opening a new parallel Range connection for the upper half.

#### 2. Evidence Supporting Finding
- Virtual address: `0x00559d00` (`ChunksList::FindMaxFreeChunk()`).
- Call sites: `0x0055b0aa`, `0x005607a1`.
- Threshold check: `0x005607cf` (`cmp dword [ebp - 0x24], 0x5b43`).
- Object allocation: `0x00560806` (`push 0x70; call operator new` allocating 112-byte `ChListItem`).

#### 3. Corresponding qtIDM Source Files and Functions
- File: [`src/core/CurlEpollDownloader.cpp`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.cpp#L709-L721)
- Functions: `CurlEpollDownloader::addTransfer()`, `CurlEpollDownloader::startQueuedSegments()`, `CurlEpollDownloader::refillQueuedSegments()`.

#### 4. Existing Implementation Details
qtIDM pre-calculates dynamic parts at initiation:
$$\text{dynamicParts} = \min\left(requestedSegments \times 4, \max\left(1, \frac{\text{total} + \text{partSize} - 1}{\text{partSize}}\right)\right)$$
It enqueues these parts into `batch->segments`. When a transfer completes, `refillQueuedSegments()` starts the next queued segment. However, if all parts have been allocated and one connection is throttled or lagging, qtIDM cannot re-split the active connection.

#### 5. Identified Differences
qtIDM lacks mid-flight segment splitting. When downloading large files from servers that throttle single streams, qtIDM suffers from "tail starvation" when 7 connections finish and the download waits on 1 slow connection.

#### 6. Potential Improvements
Implement mid-flight active segment splitting in `CurlEpollDownloader`:
- When a segment finishes and the active connection count is below the configured limit, scan active running segments for the largest unread byte span ($end - (start + written)$).
- If the unread span exceeds a minimum threshold (e.g. 256 KiB or 1 MiB), truncate the running segment's end to the midpoint and spawn a new `SegmentInfo` for $[mid + 1, end]$.

#### 7. Technical Complexity
Medium. Requires synchronizing curl easy handle management with `SparseFileWriter` boundaries and database segment updates.

#### 8. Recommended Priority
**Priority 1 (Highest Impact on Throughput).**

---

### 2.2 File Allocation & Writing Mechanisms (Superiority Assessment)

#### 1. IDM Behaviour
IDM creates individual temporary files for each connection under the user's temporary directory (`DMCache`). Upon download completion, IDM enters an "Assembling" phase where it reads every byte from all partial files and appends them sequentially into the target destination file.

#### 2. Evidence Supporting Finding
- Strings: `"An error has occurred during assembling the file"`, `"If you have several physical drives on your computer, you should select different physical drives for temporary directory and 'Save To' folders for faster assembling of downloaded files."`
- Registry keys: `Software\DownloadManager\%ld\ChList` with `sk%d` tracking partial files.

#### 3. Corresponding qtIDM Source Files and Functions
- File: [`src/core/SparseFileWriter.cpp`](file:///home/ameya/qtIDM/src/core/SparseFileWriter.cpp#L24-L89)
- Functions: `SparseFileWriter::open()`, `SparseFileWriter::writeAt()`, `SparseFileWriter::map()`.

#### 4. Existing Implementation Details
qtIDM allocates a single sparse `.part` file directly at the target location using `ftruncate()`. Worker threads write directly into memory-mapped 64 MiB sliding windows (`mmap(PROT_READ | PROT_WRITE, MAP_SHARED)`) with `pwrite` fallback.

#### 5. Identified Differences
qtIDM completely avoids file concatenation overhead. When a multi-gigabyte file completes, qtIDM executes an atomic `rename()` in sub-millisecond time, whereas IDM must read and rewrite the entire file.

#### 6. Potential Improvements
Preserve qtIDM's existing sparse file design; it is fundamentally superior to IDM's legacy architecture.

#### 7. Technical Complexity
None (already implemented).

#### 8. Recommended Priority
**Maintain Existing Architecture.**

---

### 2.3 Pre-Redirect Cookie & Context Forwarding (High Priority)

#### 1. IDM Behaviour
When an intercepted download undergoes HTTP redirects (e.g., download gateway $\to$ auth check $\to$ CDN URL), IDM's extension queries cookies for **both** the initial URL and the final target URL (`Promise.all([U(e), U(a.url)])`). It forwards the target cookie in tag `[51]` and the original pre-redirect cookie in tag `[56]`.

#### 2. Evidence Supporting Finding
- `background.js` line 921: `Promise.all([l, e]).then(t => lc(this, a, ...t))`
- Line 936: `b.finalUrl != b.url && (c[17] = dc(b.url), c[56] = V(d))`

#### 3. Corresponding qtIDM Source Files and Functions
- Extension: [`browser/chrome/service_worker.js`](file:///home/ameya/qtIDM/browser/chrome/service_worker.js#L395-L408) (`makeDownloadEntry()`)
- Downloader: [`src/core/CurlEpollDownloader.cpp`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.cpp#L1032) (`applyRequestOptions()`)

#### 4. Existing Implementation Details
qtIDM collects cookies only for the final download URL via `chrome.cookies.getAll({ url })`. If an intermediate server set session tokens or required gateway cookies, those cookies are not forwarded to libcurl.

#### 5. Identified Differences
qtIDM can fail to authenticate on services that validate session cookies tied to the top-level origin across cross-domain redirects.

#### 6. Potential Improvements
- In `browser/chrome/service_worker.js`, if `item.url !== item.finalUrl`, query cookies for both `item.url` and `item.finalUrl` and merge them or forward `initialCookies` in the download request payload.
- In `CurlEpollDownloader`, pass both cookie jars or combined cookies to `CURLOPT_COOKIE`.

#### 7. Technical Complexity
Low.

#### 8. Recommended Priority
**Priority 2 (High Reliability Improvement).**

---

### 2.4 In-Page Media API Hooking (Medium Priority)

#### 1. IDM Behaviour
IDM injects `document.js` into the page execution context via `web_accessible_resources`. It overrides `window.fetch`, `XMLHttpRequest.prototype.open`/`send`, and `Response.prototype` methods to capture streaming manifests (`.m3u8`, `.mpd`) loaded dynamically via JavaScript before they are converted into internal `blob:` URLs.

#### 2. Evidence Supporting Finding
- `research/extracted/crx/document.js`: Hooks `q[V]` (`globalThis.fetch`), wraps `Response.prototype`, decodes stream chunks with `TextDecoder`, and transmits discovered manifest URLs to `content.js` via `window.postMessage([1229212980, ...], "/")`.

#### 3. Corresponding qtIDM Source Files and Functions
- Extension: [`browser/chrome/service_worker.js`](file:///home/ameya/qtIDM/browser/chrome/service_worker.js#L532-L569) (`webRequest.onBeforeRequest`, `onHeadersReceived`)

#### 4. Existing Implementation Details
qtIDM observes network requests purely through the extension `chrome.webRequest` API.

#### 5. Identified Differences
Modern web video players (YouTube, Twitch, Netflix, custom players) often fetch video chunks or manifest fragments inside web workers or construct manifests dynamically in JavaScript memory, exposing only a `blob:http://...` URL to the `<video>` element. `chrome.webRequest` cannot inspect the contents of `blob:` URLs, meaning qtIDM misses some dynamically generated manifests that IDM detects.

#### 6. Potential Improvements
Add an optional page-context content bridge script in qtIDM that hooks `window.fetch` and `XMLHttpRequest` to detect manifests and stream URLs generated in memory.

#### 7. Technical Complexity
Medium.

#### 8. Recommended Priority
**Priority 3 (Medium Priority for Media Sniffing).**

---

### 2.5 HTTP/2 Stream Multiplexing across Ranges (Medium Priority)

#### 1. IDM Behaviour
IDM includes an HTTP/2 protocol engine that multiplexes concurrent byte ranges over a single TCP connection when connected to an HTTP/2-enabled server, reducing TCP handshake and TLS negotiation overhead.

#### 2. Evidence Supporting Finding
- Disassembly strings in `IDMan.exe`: `SETTINGS frame contained more than the maximum allowed entries`, `request HEADERS: max concurrent streams exceeded`, `PRIORITY_UPDATE`.

#### 3. Corresponding qtIDM Source Files and Functions
- File: [`src/core/CurlEpollDownloader.cpp`](file:///home/ameya/qtIDM/src/core/CurlEpollDownloader.cpp#L1012-L1045) (`addSegmentTransfer()`)

#### 4. Existing Implementation Details
qtIDM uses libcurl multi handles. By default, each curl easy handle opens its own connection unless curl's multi handle multiplexing is explicitly enabled.

#### 5. Identified Differences
qtIDM does not set `CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` on its `CURLM*` handle, nor does it explicitly request `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2_0` on easy handles.

#### 6. Potential Improvements
- Enable `curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);`
- Set `curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);`
- Enable `CURLOPT_PIPEWAIT` where appropriate so curl reuses established HTTP/2 connections for subsequent segments.

#### 7. Technical Complexity
Low to Medium.

#### 8. Recommended Priority
**Priority 4 (Connection Efficiency).**

