# 06 - Implementation Roadmap

**Document Status:** Approved Engineering Plan  
**Project:** qtIDM  
**Date:** 2026-10-30  

---

## 1. Guiding Principles & Safety Constraints

1. **Non-Destructive Development:** Preserve qtIDM's existing Qt6/C++20 modular architecture.
2. **Backward Compatibility:** All existing database schemas (`qtidm.db`), queue configuration formats (`queue.json`), and browser Native Messaging APIs must remain 100% backward-compatible.
3. **Independent Interoperability:** Clean-room implementation of algorithms based on verified functional behaviors; no proprietary code is reused.
4. **Verification Gates:** Every phase requires complete passage of the full CTest suite (`17/17` tests) plus newly introduced unit/integration tests before progressing.

---

## 2. Phased Implementation Roadmap

```mermaid
flowchart LR
    PhaseA["Phase A: Pre-Redirect Cookies\n(Browser Extension)"]
    PhaseB["Phase B: HTTP/2 Tuning\n(libcurl Multiplexing)"]
    PhaseC["Phase C: Dynamic Splitting\n(Downloader Engine)"]
    PhaseD["Phase D: In-Page Media Bridge\n(Extension Hooking)"]

    PhaseA --> TestA["Verify Cookie Tests\n(browser_interception_test)"]
    TestA --> PhaseB
    PhaseB --> TestB["Verify CTest Suite\n(DownloaderIntegrationTest)"]
    TestB --> PhaseC
    PhaseC --> TestC["Verify CTest Suite\n(DynamicSplit Tests)"]
    TestC --> PhaseD
    PhaseD --> FinalGate["Full Regression Pass\n(19/19 Tests)"]
```

---

### Milestone 1: Multi-Origin Cookie Collection (Phase A)
- **Goal:** Enhance browser extension download interception to forward both initial and final cookies across redirects.
- **Affected Components:**
  - `browser/chrome/service_worker.js`
  - `browser/firefox/background.js`
  - `src/core/CurlEpollDownloader.cpp`
- **Tasks:**
  1. Update `redirectBrowserDownload()` in `browser/chrome/service_worker.js` to detect when `item.url !== item.finalUrl`.
  2. Implement parallel cookie retrieval across both origins.
  3. Ensure cookies are merged or partitioned without leaking across disparate third-party domains.
  4. Run `browser_interception_test.cjs`.
- **Exit Gate:** All cookie tests pass; no regressions in single-URL downloads.

---

### Milestone 2: HTTP/2 Stream Multiplexing in libcurl (Phase B)
- **Goal:** Enable HTTP/2 connection reuse and stream multiplexing for Range segments where supported by upstream servers.
- **Affected Components:**
  - `src/core/CurlEpollDownloader.cpp`
  - `src/core/CurlEpollDownloader.h`
- **Tasks:**
  1. Set `curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX)` during engine initialization.
  2. Configure `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2_0` and `CURLOPT_PIPEWAIT = 1L` on easy handles.
  3. Implement fallback to HTTP/1.1 if `CURLE_HTTP2` or stream errors occur during transfer.
  4. Run `qtIDM_downloader_integration_tests`.
- **Exit Gate:** Verified HTTP/2 multiplexing test passes without affecting HTTP/1.1 compatibility.

---

### Milestone 3: Dynamic Active-Segment Re-Splitting (Phase C)
- **Goal:** Implement IDM-style `FindMaxFreeChunk` mid-stream splitting to eliminate tail latency.
- **Affected Components:**
  - `src/core/CurlEpollDownloader.h`
  - `src/core/CurlEpollDownloader.cpp`
  - `tests/DownloaderIntegrationTest.cpp`
- **Tasks:**
  1. Add `splitLargestActiveSegment(std::shared_ptr<DownloadBatch> batch)` in `CurlEpollDownloader`.
  2. Locate the active in-progress segment with the largest remaining unread byte count.
  3. If remaining unread span $\ge 256\text{ KiB}$, split at the midpoint into two sub-ranges.
  4. Safely update `batch->segments` and commit the new range to SQLite (`DownloadRepository::updateSegments`).
  5. Spawn a new curl transfer for the second sub-range via `addSegmentTransfer()`.
  6. Add unit test simulating a severely throttled tail connection and verifying automatic split and fast completion.
- **Exit Gate:** `DownloaderIntegrationTest` passes cleanly; zero data corruption on large test transfers.

---

### Milestone 4: In-Page Streaming Media Detection Bridge (Phase D)
- **Goal:** Capture streaming manifests dynamically created in JavaScript page memory.
- **Affected Components:**
  - `browser/chrome/manifest.json`
  - `browser/chrome/content_bridge.js`
  - `browser/firefox/manifest.json`
  - `browser/firefox/content_bridge.js`
- **Tasks:**
  1. Implement a lightweight, non-intrusive hook in `content_bridge.js` that intercepts `window.fetch` and `XMLHttpRequest.prototype.open`.
  2. Detect `.m3u8` and `.mpd` signatures.
  3. Post detected manifest URLs to `content_bridge.js` via secure `window.postMessage`.
  4. Ensure complete isolation from site scripts.
  5. Test on dynamic HTML5 test pages.
- **Exit Gate:** Stream detection verified in browser tests without breaking site execution.

---

## 3. Verification & Regression Plan

Before any proposed code change is submitted:
1. Run the full local test suite: `ctest --test-dir build --output-on-failure`.
2. Verify all 17 existing automated test suites pass.
3. Verify newly introduced tests pass.
4. Verify database backward compatibility: launch `qtIDM` against an existing `qtidm.db` database and verify that existing downloads, segments, and history load without error.
5. Verify browser extension packaging via `packaging/browser/build-extensions.sh`.

