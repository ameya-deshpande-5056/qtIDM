# 07 - Evidence Ledger and Technical References

**Document Status:** Complete Evidence Ledger  
**Project:** IDM Reverse Engineering & qtIDM Capability Research  
**Date:** 2026-10-30  

---

## 1. Analyzed Binary & Package Identifiers

| Artifact | File Path | File Size | SHA-256 Checksum | Confidence Level |
|---|---|---|---|---|
| **IDM Installer** | `/home/ameya/Downloads/idman643build16.exe` | 12,698,904 bytes | `276aa7e4ff7682445b011ebe6ac3920e7eca6ece7d3f6723b1f1cb515164461b` | 100% (Verified) |
| **IDM Extension** | `/home/ameya/Downloads/NGPAMPAPPNMEPGILOJFOHADHHMBHLAEK_6_43_1_0.crx` | 107,834 bytes | `1d2297ccbdbbf06e9e2546bb9f9b9985433eb7c02d75f09bbcbc9784ece583f7` | 100% (Verified) |
| **Cutter AppImage**| `/home/ameya/Downloads/Cutter-v2.5.0-Linux-x86_64.AppImage` | 144,423,416 bytes | `b8ad215d7a9e2af9e1f463511229f16e1f4745a0fb541413e5f4787f949ac0cf` | 100% (Verified) |
| **Extracted IDMan.exe** | `research/extracted/idm_binaries/IDMan.exe` | 6,500,432 bytes | `9a35e4e7ff531d2797e87a2dfb4260904d9c02ff3cf28670daef5a1768652431` | 100% (Verified) |
| **Extracted IDMMsgHost.exe** | `research/extracted/idm_binaries/IDMMsgHost.exe` | 39,168 bytes | `13a7c6453db47ebec1823db91ec1b7dfb0798be10e5aa326e1180295eb48421b` | 100% (Verified) |

---

## 2. Key Binary Offsets, Symbols, and Addresses in `IDMan.exe`

| Virtual Address | Raw Offset | Symbol / Disassembly Context | Evidence Type | Confidence Level |
|---|---|---|---|---|
| `0x00559d00` | `0x159100` | `ChunksList::FindMaxFreeChunk` entry point | Static Disassembly | 100% (Observed Fact) |
| `0x00559ee7` | `0x1592e7` | String push: `"Unknown error during ChunksList::FindMaxFreeChunk()"` | Static Disassembly | 100% (Observed Fact) |
| `0x0055b0aa` | `0x15a4aa` | Call site 1 to `FindMaxFreeChunk` (connection completion) | Static Disassembly | 100% (Observed Fact) |
| `0x005607a1` | `0x15fba1` | Call site 2 to `FindMaxFreeChunk` (dynamic connection splitting) | Static Disassembly | 100% (Observed Fact) |
| `0x005607cf` | `0x15fbcf` | Comparison against `0x5b43` (23,363 bytes minimum split threshold) | Static Disassembly | 100% (Observed Fact) |
| `0x00560806` | `0x15fc06` | `operator new(0x70)` allocating 112-byte `ChListItem` chunk | Static Disassembly | 100% (Observed Fact) |
| `0x00566480` | `0x165880` | `ChListItem::savesk` entry point | Static Disassembly | 100% (Observed Fact) |
| `0x005664e1` | `0x1658e1` | `RegSetValueExA(hKey, "sk%d", ..., REG_DWORD, ...)` | Static Disassembly | 100% (Observed Fact) |
| `0x00566540` | `0x165940` | `ChListItem::getsk` entry point (`RegQueryValueExA`) | Static Disassembly | 100% (Observed Fact) |
| `0x00707198` | `0x305d98` | String: `"Software\DownloadManager\%ld\ChList"` | String Table | 100% (Observed Fact) |
| `0x006ddf3c` | `0x2dcb3c` | String: `"%s%sDMCache\%s"` (temporary partial cache path) | String Table | 100% (Observed Fact) |
| `0x007c5028` | `0x3c3428` | RTTI: `.?AVCIDMCacheDatabase@@` | String Table | 100% (Observed Fact) |
| `0x007088d0` | `0x3074d0` | String: `"Unkerr in ChListItem::savesk\n"` | String Table | 100% (Observed Fact) |
| `0x007088f8` | `0x3074f8` | String: `"Unkerr in ChListItem::getsk\n"` | String Table | 100% (Observed Fact) |

---

## 3. Extension Protocol Evidence (`background.js` / `content.js`)

| Source Reference | Code Pattern / String | Semantic Functionality | Confidence Level |
|---|---|---|---|
| `background.js:66` | `La=["127.0.0.1:1001","0.1.0.1:1001"]` | Localhost WebSocket endpoints | 100% (Observed Fact) |
| `background.js:576` | `new WebSocket("ws://127.0.0.1:1001/...", "plugin.v3.internetdownloadmanager.com")` | Primary WebSocket IPC connection | 100% (Observed Fact) |
| `background.js:576` | `browser.runtime.connectNative("com.tonec.idm")` | Native Messaging fallback | 100% (Observed Fact) |
| `background.js:77` | `["MSG#", a, "#", b, "#", c, "#", d]` | Proprietary ASCII framing format | 100% (Observed Fact) |
| `background.js:880` | `n.qb = function(a)` | Immediate pause in `downloads.onCreated` | 100% (Observed Fact) |
| `background.js:900` | `n.rb = function(a, b)` | Rule validation & handoff in `onDeterminingFilename` | 100% (Observed Fact) |
| `background.js:921` | `Promise.all([U(e), U(a.url)])...` | Capturing cookies for both original and final URLs | 100% (Observed Fact) |
| `background.js:933` | `[6]: dc(b.finalUrl), [50]: b.referrer, [51]: V(c), [56]: V(d)` | Attribute tag mapping | 100% (Observed Fact) |
| `document.js:1` | Hooking `window.fetch`, `XHR.prototype.open`, `Response.prototype` | In-page stream manifest sniffing | 100% (Observed Fact) |

---

## 4. Distinction of Findings by Evidence Classification

### Category A: Observed Facts (Direct Binary & Source Verification)
- IDM uses raw Winsock2 (`ws2_32.dll`) non-blocking sockets multiplexed with `select()`.
- IDM uses a proprietary ASCII frame protocol `MSG#seq#type#subType#flags...;`.
- IDM implements a dual-channel IPC transport (Localhost WebSocket on port 1001 with fallback to Native Messaging).
- IDM writes separate files per connection in `DMCache` and concatenates them upon completion ("Assembling").
- IDM stores download configuration and channel status (`sk%d`) in the Windows Registry (`Software\DownloadManager`).
- IDM implements dynamic active-chunk splitting (`FindMaxFreeChunk` at `0x00559d00` with threshold `23,363` bytes).
- IDM captures cookies across redirects for both initial and final target URLs.
- qtIDM uses a single sparse file (`.part`) via sliding `mmap` windows with atomic `rename` upon completion.
- qtIDM uses SQLite 3 in WAL mode for persistent download and segment state.

### Category B: Static Analysis Deductions (High Confidence)
- IDM's internal HTTP/2 engine (`SETTINGS`, `HEADERS`, `PRIORITY_UPDATE`) is used to multiplex Range requests across a single TCP socket when the server supports HTTP/2.
- IDM's recommendation that users place the temporary directory and download directory on different physical disks is an architectural workaround for the I/O bottleneck caused by its file concatenation step.

### Category C: Unverified Hypotheses & Out-of-Scope Items
- Exact performance difference of IDM's raw Winsock `select()` loop vs. qtIDM's libcurl multi `epoll` engine on 10 Gbps networks (would require Windows kernel instrumentation and live comparative throughput benchmarking).
- Whether IDM's TDI / WFP kernel drivers (`idmwfp.sys`, `idmtdi.sys`) perform any packet injection beyond browser socket redirection (out of scope, non-applicable to Linux).

