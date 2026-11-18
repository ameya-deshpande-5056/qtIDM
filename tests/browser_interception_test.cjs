"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ROOT = path.resolve(__dirname, "..");

function event() {
  const listeners = [];
  const filters = [];
  return {
    listeners,
    filters,
    addListener(listener, filter) {
      listeners.push(listener);
      filters.push(filter);
    }
  };
}

function browserApi(rootName) {
  const calls = [];
  const nativeMessages = [];
  let nativeError = null;
  let nativeDownloadError = null;
  const api = {
    runtime: {
      id: "qtidm-test-extension",
      onInstalled: event(),
      onMessage: event(),
      async sendMessage() { return { ok: true }; },
      async sendNativeMessage(_host, message) {
        calls.push("native");
        nativeMessages.push(message);
        if (nativeError) throw nativeError;
        if (nativeDownloadError && message.prepare !== true) throw nativeDownloadError;
        return { ok: true, prepared: message.prepare === true, accepted: message.prepare !== true };
      }
    },
    storage: {
      local: {
        async get() { return {}; },
        async set() {},
        async remove() {}
      }
    },
    contextMenus: {
      create() {},
      onClicked: event()
    },
    downloads: {
      onCreated: event(),
      async pause() { calls.push("pause"); },
      async resume() { calls.push("resume"); },
      async cancel() { calls.push("cancel"); },
      async removeFile() { calls.push("removeFile"); },
      async erase() { calls.push("erase"); },
      async download(options) {
        calls.push("download");
        for (const listener of api.downloads.onCreated.listeners) {
          listener({ id: 99, url: options.url });
        }
        return 99;
      }
    },
    cookies: {
      mockJar: [],
      async getAll(options = {}) {
        const url = options.url;
        if (!url || !api.cookies.mockJar.length) return api.cookies.mockJar || [];
        try {
          const target = new URL(url);
          const host = target.hostname.toLowerCase();
          return api.cookies.mockJar.filter((c) => {
            let domain = (c.domain || "").toLowerCase();
            if (domain.startsWith(".")) domain = domain.slice(1);
            if (c.hostOnly) return host === domain;
            return host === domain || host.endsWith("." + domain);
          });
        } catch (_) {
          return [];
        }
      }
    },
    tabs: {
      onRemoved: event(),
      async get() { return { url: "https://example.test/page" }; },
      async query() { return [{ id: 101, active: true, url: "https://video.example.test/watch" }]; },
      ...(rootName === "browser" ? {
        async executeScript(_tabId, _options) {
          return [["https://example.test/a.pdf", "https://example.test/b.pdf"]];
        }
      } : {})
    },
    ...(rootName === "chrome" ? {
      scripting: {
        async executeScript() {
          return [{ result: ["https://example.test/a.pdf", "https://example.test/b.pdf"] }];
        }
      }
    } : {}),
    webRequest: {
      onBeforeRequest: event(),
      onBeforeSendHeaders: event(),
      onHeadersReceived: event()
    }
  };
  api[rootName === "chrome" ? "action" : "browserAction"] = {
    async setBadgeText() {},
    async setTitle() {}
  };
  return {
    api,
    calls,
    nativeMessages,
    failNative(error) {
      nativeError = error;
    },
    failNativeAfterPrepare(error) {
      nativeDownloadError = error;
    }
  };
}

async function testVariant(relativePath, rootName) {
  const mock = browserApi(rootName);
  const context = {
    [rootName]: mock.api,
    URL,
    navigator: { userAgent: "qtIDM interception test" },
    btoa(value) { return Buffer.from(value, "binary").toString("base64"); },
    unescape,
    console: { error() {} }
  };
  vm.createContext(context);
  vm.runInContext(
    fs.readFileSync(path.join(ROOT, relativePath), "utf8"),
    context,
    { filename: relativePath }
  );

  for (const eventName of ["onBeforeSendHeaders", "onHeadersReceived"]) {
    const types = mock.api.webRequest[eventName].filters[0]?.types || [];
    assert.ok(
      types.includes("main_frame") && types.includes("sub_frame"),
      `${rootName} must observe headers for downloads initiated by frame navigation`
    );
  }

  assert.equal(
    context.responseFilename({
      responseHeaders: [
        { name: "Content-Disposition", value: "attachment; filename*=UTF-8''Persona%205%20Episode%2001.mp4" }
      ]
    }),
    "Persona 5 Episode 01.mp4"
  );
  assert.equal(
    context.mediaSuggestedFilename({}, { title: "Persona 5 Episode 01 :: Kwik" }),
    "Persona 5 Episode 01"
  );
  assert.equal(
    context.mediaSuggestedFilename({}, { title: "Steel Ball Run: JoJo's Bizarre Adventure Ep. 7 :: animepahe" }),
    "Steel Ball Run - JoJo's Bizarre Adventure Ep. 7"
  );
  assert.equal(
    context.mediaSuggestedFilename({}, { title: "Fate/stay night [Unlimited Blade Works] - Ep. 01" }),
    "Fate - stay night [Unlimited Blade Works] - Ep. 01"
  );
  assert.equal(
    context.mediaSuggestedFilename({}, { title: "Dr. STONE Season 3 Ep. 12" }),
    "Dr. STONE Season 3 Ep. 12"
  );
  assert.equal(
    context.mediaSuggestedFilename({ suggestedFilename: "uwu.m3u8" }, { title: "Steel Ball Run: JoJo's Bizarre Adventure Ep. 7 :: animepahe" }),
    "Steel Ball Run - JoJo's Bizarre Adventure Ep. 7"
  );
  assert.equal(
    context.mediaSuggestedFilename({}, { title: "Episode 01." }),
    "Episode 01"
  );
  assert.equal(
    context.mediaSuggestedFilename({ url: "https://example.test/stream/episode1.mp4" }, { title: "Persona 5 Episode 01" }),
    "Persona 5 Episode 01.mp4",
    "mediaSuggestedFilename must retain video extension from URL"
  );
  assert.equal(
    context.mediaSuggestedFilename({ type: "Audio" }, { title: "Podcast Episode 10" }),
    "Podcast Episode 10.mp3",
    "mediaSuggestedFilename must provide audio extension based on detected media type"
  );
  assert.equal(
    context.browserSuggestedFilename("document_without_ext", "https://example.test/files/manual.pdf"),
    "document_without_ext.pdf",
    "browserSuggestedFilename must preserve extension from URL if captured filename lacks one"
  );

  mock.api.webRequest.onBeforeSendHeaders.listeners[0]({
    requestId: "observed-request",
    tabId: 1,
    url: "https://example.test/archive.bin",
    requestHeaders: [
      { name: "Origin", value: "https://example.test" },
      { name: "Referer", value: "https://example.test/page" },
      { name: "Sec-Fetch-Site", value: "same-origin" }
    ]
  });
  await context.sendToHost(
    "https://example.test/archive.bin",
    { url: "https://fallback.invalid/" },
    "",
    {},
    "HLS",
    "Browser supplied name.mp4"
  );
  const sent = mock.nativeMessages.at(-1).downloads[0];
  assert.equal(sent.headers._qtidmMediaType, "HLS");
  assert.equal(sent.suggestedFilename, "Browser supplied name.mp4");
  assert.equal(sent.headers.Origin, "https://example.test");
  assert.equal(sent.headers.Referer, "https://example.test/page");
  assert.equal(sent.headers["Sec-Fetch-Site"], "same-origin");
  mock.calls.length = 0;

  await context.sendToHost(
    "https://cdn.example.test/fresh-stream.m3u8",
    { url: "https://player.example.test/watch/1" },
    "",
    {},
    "HLS"
  );
  const fallbackContext = mock.nativeMessages.at(-1).downloads[0];
  assert.equal(fallbackContext.headers.Referer, "https://player.example.test/watch/1");
  assert.equal(fallbackContext.headers.Origin, "https://player.example.test");
  mock.calls.length = 0;

  mock.api.webRequest.onHeadersReceived.listeners[0]({
    url: "https://example.test/archive.bin",
    responseHeaders: [
      { name: "Content-Disposition", value: 'attachment; filename="Server supplied name.mp4"' }
    ]
  });

  const item = {
    id: 7,
    referrer: "https://example.test/page",
    filename: "/home/test/AnimePahe_Persona_5_the_Animation_01_BD_1080p_nks.mp4"
  };
  await context.redirectBrowserDownload(item, "https://example.test/archive.bin");
  assert.equal(
    mock.nativeMessages.at(-1).downloads[0].suggestedFilename,
    "AnimePahe_Persona_5_the_Animation_01_BD_1080p_nks.mp4"
  );
  assert.deepEqual(
    mock.calls,
    ["pause", "native", "cancel", "removeFile", "erase", "native"],
    `${rootName} must remove the browser file before opening qtIDM's download dialog`
  );

  for (const copy of [1, 2, 37]) {
    await context.redirectBrowserDownload(
      { id: 7 + copy, referrer: "https://example.test/page", filename: `/home/test/Server supplied name(${copy}).mp4` },
      "https://example.test/archive.bin"
    );
    assert.equal(
      mock.nativeMessages.at(-1).downloads[0].suggestedFilename,
      "Server supplied name.mp4",
      `${rootName} must discard the browser's (${copy}) collision suffix`
    );
  }

  await context.redirectBrowserDownload(
    { id: 45, referrer: "https://example.test/page", filename: "" },
    "https://example.test/archive.bin"
  );
  assert.equal(mock.nativeMessages.at(-1).downloads[0].suggestedFilename, "Server supplied name.mp4");

  mock.calls.length = 0;
  mock.failNative(new Error("native host unavailable"));
  await assert.rejects(
    context.redirectBrowserDownload(item, "https://example.test/archive.bin"),
    /native host unavailable/
  );
  assert.deepEqual(
    mock.calls,
    ["pause", "native", "resume"],
    `${rootName} must resume the browser download when native messaging fails`
  );

  mock.failNative(null);
  mock.calls.length = 0;
  mock.failNativeAfterPrepare(new Error("application rejected download"));
  await assert.rejects(
    context.redirectBrowserDownload(
      { ...item, url: "https://example.test/archive.bin" },
      "https://example.test/archive.bin"
    ),
    /application rejected download/
  );
  await Promise.resolve();
  assert.deepEqual(
    mock.calls,
    ["pause", "native", "cancel", "removeFile", "erase", "native", "download"],
    `${rootName} must restore a rejected browser download without intercepting the restoration`
  );

  // Phase B: Verify RFC 6265 scoped cookie forwarding across redirect & expiry filtering
  mock.calls.length = 0;
  mock.failNativeAfterPrepare(null);
  mock.api.cookies.mockJar = [
    { name: "shared_session", value: "s123", domain: ".example.test", hostOnly: false },
    { name: "auth_private", value: "secret456", domain: "auth.example.test", hostOnly: true },
    { name: "cdn_specific", value: "c789", domain: "cdn.example.test", hostOnly: true },
    { name: "expired_token", value: "dead", domain: ".example.test", hostOnly: false, expirationDate: Math.floor(Date.now() / 1000) - 3600 }
  ];
  await context.redirectBrowserDownload(
    { id: 88, url: "https://auth.example.test/download", finalUrl: "https://cdn.example.test/file.bin", referrer: "https://example.test/" },
    "https://cdn.example.test/file.bin"
  );
  const redirectedSent = mock.nativeMessages.at(-1).downloads[0];
  const cookieHeader = redirectedSent.headers.Cookie || "";
  assert.ok(cookieHeader.includes("shared_session=s123"), "Shared domain cookie must be forwarded across redirect");
  assert.ok(cookieHeader.includes("cdn_specific=c789"), "Destination host cookie must be forwarded");
  assert.ok(!cookieHeader.includes("auth_private"), "Host-only pre-redirect cookie must NOT leak to different host");
  assert.ok(!cookieHeader.includes("expired_token"), "Expired cookie must be rejected");

  assert.ok(Array.isArray(redirectedSent.cookies), "downloads must contain structured cookies array");
  assert.ok(redirectedSent.cookies.some((c) => c.name === "shared_session" && c.value === "s123" && c.domain === ".example.test" && c.hostOnly === false), "Structured shared cookie must be preserved");
  assert.ok(redirectedSent.cookies.some((c) => c.name === "cdn_specific" && c.value === "c789" && c.hostOnly === true), "Structured host-only cookie must be preserved");
  assert.ok(!redirectedSent.cookies.some((c) => c.name === "auth_private"), "Host-only pre-redirect cookie must NOT leak to different host in structured cookies");
  assert.ok(!redirectedSent.cookies.some((c) => c.name === "expired_token"), "Expired cookie must NOT be present in structured cookies");

  // Phase D: In-Page Media Bridge (discoveredMedia message handling & scheme validation)
  assert.ok(
    context.isDirectMediaResponse({
      type: "xmlhttprequest",
      url: "https://example.test/stream.mp4",
      responseHeaders: [{ name: "Content-Type", value: "video/mp4" }]
    }),
    "Direct media response must recognize video/mp4 fetched via XHR/fetch"
  );
  assert.ok(
    !context.isDirectMediaResponse({
      type: "xmlhttprequest",
      url: "https://example.test/segment_1.ts",
      responseHeaders: [{ name: "Content-Type", value: "video/mp2t" }]
    }),
    "Direct media response must not treat transport stream segments as standalone media"
  );

  const resolvedTabId = await context.resolveTabId({
    tabId: -1,
    initiator: "https://video.example.test"
  });
  assert.equal(resolvedTabId, 101, "Decoupled media request must resolve active tab by initiator origin");

  for (const listener of mock.api.runtime.onMessage.listeners) {
    let responseResult = null;
    await listener(
      { type: "discoveredMedia", url: "https://video.example.test/live/stream.m3u8", mediaType: "HLS", referrer: "https://video.example.test/watch" },
      { tab: { id: 101 } },
      (res) => { responseResult = res; }
    );
    // Invalid schemes must be dropped cleanly without error
    await listener(
      { type: "discoveredMedia", url: "javascript:alert(1)", mediaType: "HLS" },
      { tab: { id: 101 } },
      (res) => { responseResult = res; }
    );
    await listener(
      { type: "listMedia", tabId: 101 },
      {},
      (res) => { responseResult = res; }
    );
    mock.calls.length = 0;
    mock.nativeMessages.length = 0;
    await new Promise((resolve) => {
      listener(
        { type: "extractSocialMedia", tabId: 101, url: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" },
        {},
        (res) => { responseResult = res; resolve(); }
      );
    });
    assert.equal(responseResult?.ok, true);
    assert.equal(mock.nativeMessages.at(-1)?.type, "extractSocialMedia");
    assert.equal(mock.nativeMessages.at(-1)?.url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ");
  }

  // Phase E: Verify dynamic MIME-to-extension resolution across containers without hardcoding
  assert.equal(context.extensionFromMime("audio/opus"), "opus");
  assert.equal(context.extensionFromMime("audio/webm; codecs=\"opus\""), "webm");
  assert.equal(context.extensionFromMime("video/webm; codecs=\"vp9\""), "webm");
  assert.equal(context.extensionFromMime("audio/flac"), "flac");
  assert.equal(context.extensionFromMime("audio/x-m4a"), "m4a");
  assert.equal(context.extensionFromMime("audio/mp4"), "m4a");
  assert.equal(context.extensionFromMime("audio/aac"), "aac");
  assert.equal(context.extensionFromMime("video/quicktime"), "mov");
  assert.equal(context.extensionFromMime("video/x-matroska"), "mkv");

  const opusItem = {
    url: "https://example.test/stream?id=audio-only",
    type: "Audio",
    contentType: "audio/opus"
  };
  assert.equal(context.mediaExtension(opusItem), "opus", "Opus audio stream without file extension in URL must resolve to .opus");
  assert.equal(
    context.mediaSuggestedFilename(opusItem, { title: "Podcast Deep Dive" }),
    "Podcast Deep Dive.opus",
    "Suggested filename for Opus audio must be .opus, never hardcoded to .mp3"
  );

  const webmItem = {
    url: "https://example.test/videoplayback?expire=999",
    type: "Video",
    contentType: "video/webm; codecs=\"vp9\""
  };
  assert.equal(context.mediaExtension(webmItem), "webm", "WebM video stream without file extension in URL must resolve to .webm");
  assert.equal(
    context.mediaSuggestedFilename(webmItem, { title: "4K Nature Documentary" }),
    "4K Nature Documentary.webm",
    "Suggested filename for WebM video must be .webm, never hardcoded to .mp4"
  );

  const flacItem = {
    url: "https://example.test/music/song.flac",
    type: "Audio",
    contentType: "audio/flac"
  };
  assert.equal(context.mediaExtension(flacItem), "flac", "FLAC stream must retain .flac extension");
  assert.equal(
    context.mediaSuggestedFilename(flacItem, { title: "Lossless Track" }),
    "Lossless Track.flac",
    "Suggested filename for FLAC audio must retain .flac"
  );

  // Mismatched title extension (e.g. author typed .mp4 in title, but stream is actually webm or opus)
  assert.equal(
    context.mediaSuggestedFilename(webmItem, { title: "Nature.mp4" }),
    "Nature.webm",
    "Mismatched container extension in title must be replaced with true stream container to prevent file corruption"
  );

  const downloadEntry = await context.makeDownloadEntry(
    "https://example.test/audio-stream",
    { url: "https://example.test/page" },
    "",
    {},
    "Audio",
    "Podcast.opus",
    "GET",
    "",
    "",
    "audio/opus"
  );
  assert.equal(downloadEntry.headers._qtidmMimeType, "audio/opus", "makeDownloadEntry must pass _qtidmMimeType");
  assert.equal(downloadEntry.headers._qtidmMediaType, "Audio", "makeDownloadEntry must pass _qtidmMediaType");

  // --- Blob & Object URL Interception Tests ---
  // 1. isExcluded with blob: URL
  assert.equal(
    context.isExcluded("blob:https://excluded.test/550e8400-e29b-41d4-a716-446655440000", { excludedHosts: "excluded.test" }),
    true,
    "isExcluded must correctly inspect hostname from blob: URLs"
  );
  assert.equal(
    context.isExcluded("blob:https://allowed.test/550e8400-e29b-41d4-a716-446655440000", { excludedHosts: "excluded.test" }),
    false,
    "isExcluded must not exclude non-matching blob: URLs"
  );

  // 2. mediaExtension for Document and Archive blobs
  assert.equal(context.mediaExtension({ type: "Document" }), "pdf", "mediaExtension must resolve Document to pdf");
  assert.equal(context.mediaExtension({ type: "Archive" }), "zip", "mediaExtension must resolve Archive to zip");
  assert.equal(context.mediaExtension({ type: "Video", suggestedFilename: "clip.webm" }), "webm", "mediaExtension must honor suggestedFilename extension");

  // 3. storeMedia with blob: URL
  const testStorage = {};
  mock.api.storage.local.get = async (k) => (k ? { [k]: testStorage[k] } : testStorage);
  mock.api.storage.local.set = async (obj) => Object.assign(testStorage, obj);
  if (mock.api.storage.session) {
    mock.api.storage.session.get = mock.api.storage.local.get;
    mock.api.storage.session.set = mock.api.storage.local.set;
  }

  const blobUrl = "blob:https://video.example.test/7c9e6679-7425-40de-944b-e07fc1f90ae7";
  await context.storeMedia({
    tabId: 101,
    url: blobUrl,
    isBlob: true,
    size: 52428800,
    mimeType: "video/mp4",
    filename: "DecryptedStream.mp4",
    mediaType: "Video"
  }, { captureMedia: true, excludedHosts: "", excludedExtensions: "" });

  const tabMediaKey = `qtidm-media-101`;
  const storedItems = testStorage[tabMediaKey];
  assert.ok(Array.isArray(storedItems) && storedItems.length > 0, "storeMedia must store blob: URLs in captureStorage");
  const storedBlob = storedItems[0];
  assert.equal(storedBlob.url, blobUrl);
  assert.equal(storedBlob.isBlob, true);
  assert.equal(storedBlob.size, 52428800);
  assert.equal(storedBlob.suggestedFilename, "DecryptedStream.mp4");
  assert.equal(storedBlob.type, "Video");

  // 4. downloadMedia with blob: URL (must use browser.downloads.download, not sendToHost)
  mock.calls.length = 0;
  let downloadedOptions = null;
  mock.api.downloads.download = async (opt) => {
    mock.calls.push("download");
    downloadedOptions = opt;
    return 102;
  };
  mock.api.tabs.get = async () => ({ id: 101, title: "Video Player Page", url: "https://video.example.test/watch" });

  let downloadResponse = null;
  for (const listener of mock.api.runtime.onMessage.listeners) {
    const replyPromise = new Promise((resolve) => {
      let resolved = false;
      const done = (val) => { if (!resolved) { resolved = true; downloadResponse = val; resolve(val); } };
      const res = listener({
        type: "downloadMedia",
        tabId: 101,
        index: 0,
        url: blobUrl
      }, {}, done);
      if (res instanceof Promise) res.then(done);
    });
    await replyPromise;
  }

  assert.deepEqual(mock.calls, ["download"], "downloadMedia for blob: URL must invoke browser.downloads.download directly");
  assert.equal(downloadedOptions.url, blobUrl);
  assert.equal(downloadedOptions.filename, "DecryptedStream.mp4");
  assert.ok(downloadResponse && downloadResponse.ok, "downloadMedia must return ok: true for blob download");

  // 5. downloadMedia with revoked blob URL and automatic refresh
  const revokedBlobUrl = "blob:https://video.example.test/revoked-uuid";
  await context.storeMedia({
    tabId: 101,
    url: revokedBlobUrl,
    isBlob: true,
    size: 1048576,
    filename: "Report.pdf",
    mediaType: "Document"
  }, { captureMedia: true });

  const freshBlobUrl = "blob:https://video.example.test/refreshed-uuid";
  mock.calls.length = 0;
  let downloadAttempts = 0;
  mock.api.downloads.download = async (opt) => {
    mock.calls.push("download");
    downloadAttempts++;
    if (opt.url === revokedBlobUrl) {
      throw new Error("ERR_FILE_NOT_FOUND");
    }
    return 103;
  };
  mock.api.tabs.sendMessage = async (_tabId, msg) => {
    if (msg.type === "refreshBlobUrl" && msg.url === revokedBlobUrl) {
      return { ok: true, url: freshBlobUrl };
    }
    return { ok: false };
  };

  downloadResponse = null;
  for (const listener of mock.api.runtime.onMessage.listeners) {
    const replyPromise = new Promise((resolve) => {
      let resolved = false;
      const done = (val) => { if (!resolved) { resolved = true; downloadResponse = val; resolve(val); } };
      const res = listener({
        type: "downloadMedia",
        tabId: 101,
        url: revokedBlobUrl
      }, {}, done);
      if (res instanceof Promise) res.then(done);
    });
    await replyPromise;
  }

  assert.equal(downloadAttempts, 2, "downloadMedia must retry with refreshed blob URL upon revocation");
  assert.ok(downloadResponse && downloadResponse.ok, "downloadMedia with refreshed blob must succeed");

  // 6. downloadMedia Tier 3 fallback: cross-origin downloads.download blocked -> downloadBlobInPage
  const partitionedBlobUrl = "blob:https://video.example.test/partitioned-uuid";
  await context.storeMedia({
    tabId: 101,
    url: partitionedBlobUrl,
    isBlob: true,
    size: 2097152,
    filename: "EncryptedBackup.zip",
    mediaType: "Archive"
  }, { captureMedia: true });

  mock.calls.length = 0;
  let inPageRequest = null;
  mock.api.downloads.download = async () => {
    mock.calls.push("download");
    throw new Error("Cross-origin blob URL blocked");
  };
  mock.api.tabs.sendMessage = async (_tabId, msg) => {
    if (msg.type === "refreshBlobUrl") {
      return { ok: true, url: "blob:https://video.example.test/partitioned-refreshed" };
    }
    if (msg.type === "downloadBlobInPage") {
      inPageRequest = msg;
      return { ok: true };
    }
    return { ok: false };
  };

  downloadResponse = null;
  for (const listener of mock.api.runtime.onMessage.listeners) {
    const replyPromise = new Promise((resolve) => {
      let resolved = false;
      const done = (val) => { if (!resolved) { resolved = true; downloadResponse = val; resolve(val); } };
      const res = listener({
        type: "downloadMedia",
        tabId: 101,
        url: partitionedBlobUrl
      }, {}, done);
      if (res instanceof Promise) res.then(done);
    });
    await replyPromise;
  }
  assert.ok(downloadResponse && downloadResponse.ok, "downloadMedia must fall back to downloadBlobInPage when downloads.download is blocked by origin partitioning");
  assert.equal(inPageRequest?.url, "blob:https://video.example.test/partitioned-refreshed");
  assert.equal(inPageRequest?.filename, "EncryptedBackup.zip");

  // 7. Cross-Origin Iframe lifecycle: frameId/frameUrl persistence, iframe Referer/Origin, targeted frameId blob refresh & frameUnloaded pruning
  const iframeHlsUrl = "https://edge.embed.test/hls/master.m3u8";
  const iframeBlobUrl = "blob:https://embed.cdn.test/iframe-blob-42";
  for (const listener of mock.api.runtime.onMessage.listeners) {
    await new Promise((resolve) => {
      let done = false;
      const fin = () => { if (!done) { done = true; resolve(); } };
      const res = listener({
        type: "discoveredMedia",
        url: iframeHlsUrl,
        mediaType: "HLS",
        referrer: "about:blank",
        isSubframe: true,
        frameToken: "qtidm-frame-42",
        frameUrl: "https://embed.cdn.test/player/99",
        frameTitle: "Embedded Player"
      }, { tab: { id: 101, url: "https://video.example.test/watch" }, frameId: 42, url: "https://embed.cdn.test/player/99" }, fin);
      if (res instanceof Promise) res.then(fin);
    });
    await new Promise((resolve) => {
      let done = false;
      const fin = () => { if (!done) { done = true; resolve(); } };
      const res = listener({
        type: "discoveredMedia",
        url: iframeBlobUrl,
        mediaType: "Video",
        isBlob: true,
        size: 4096000,
        mimeType: "video/mp4",
        filename: "IframeClip.mp4",
        isSubframe: true,
        frameToken: "qtidm-frame-42",
        frameUrl: "https://embed.cdn.test/player/99"
      }, { tab: { id: 101, url: "https://video.example.test/watch" }, frameId: 42, url: "https://embed.cdn.test/player/99" }, fin);
      if (res instanceof Promise) res.then(fin);
    });
  }

  const iframeItems = testStorage[tabMediaKey];
  const storedIframeHls = iframeItems.find((e) => e.url === iframeHlsUrl);
  const storedIframeBlob = iframeItems.find((e) => e.url === iframeBlobUrl);
  assert.equal(storedIframeHls?.frameId, 42, "Iframe HLS capture must persist sender.frameId");
  assert.equal(storedIframeHls?.isSubframe, true, "Iframe capture must be flagged isSubframe");
  assert.equal(storedIframeHls?.referrer, "https://embed.cdn.test/player/99", "about:blank referrer must fall back to HTTP(S) frameUrl");
  assert.equal(storedIframeBlob?.frameId, 42);
  assert.equal(storedIframeBlob?.frameToken, "qtidm-frame-42");

  // Downloading iframe HLS must forward the iframe's Referer and Origin (not top tab)
  mock.nativeMessages.length = 0;
  for (const listener of mock.api.runtime.onMessage.listeners) {
    await new Promise((resolve) => {
      let done = false;
      const fin = () => { if (!done) { done = true; resolve(); } };
      const res = listener({ type: "downloadMedia", tabId: 101, url: iframeHlsUrl }, {}, fin);
      if (res instanceof Promise) res.then(fin);
    });
  }
  const iframeDownloadPayload = mock.nativeMessages.at(-1)?.downloads?.[0];
  assert.equal(iframeDownloadPayload?.headers?.Referer, "https://embed.cdn.test/player/99");
  assert.equal(iframeDownloadPayload?.headers?.Origin, "https://embed.cdn.test");

  // Downloading revoked iframe Blob must target { frameId: 42 } and pass targetFrameToken
  let capturedSendMessageArgs = null;
  mock.api.downloads.download = async (opt) => {
    if (opt.url === iframeBlobUrl) throw new Error("ERR_FILE_NOT_FOUND");
    return 104;
  };
  mock.api.tabs.sendMessage = async (tabId, msg, opts) => {
    capturedSendMessageArgs = { tabId, msg, opts };
    return { ok: true, url: "blob:https://embed.cdn.test/iframe-blob-42-fresh" };
  };
  for (const listener of mock.api.runtime.onMessage.listeners) {
    await new Promise((resolve) => {
      let done = false;
      const fin = () => { if (!done) { done = true; resolve(); } };
      const res = listener({ type: "downloadMedia", tabId: 101, url: iframeBlobUrl }, {}, fin);
      if (res instanceof Promise) res.then(fin);
    });
  }
  assert.equal(capturedSendMessageArgs?.opts?.frameId, 42, "Blob refresh for iframe capture must target owning frameId");
  assert.equal(capturedSendMessageArgs?.msg?.targetFrameToken, "qtidm-frame-42");

  // frameUnloaded must prune dead blob: capture from frame 42 while preserving live HTTP(S) HLS capture
  for (const listener of mock.api.runtime.onMessage.listeners) {
    await new Promise((resolve) => {
      let done = false;
      const fin = () => { if (!done) { done = true; resolve(); } };
      const res = listener({
        type: "frameUnloaded",
        frameToken: "qtidm-frame-42"
      }, { tab: { id: 101 }, frameId: 42 }, fin);
      if (res instanceof Promise) res.then(fin);
    });
  }
  assert.ok(!testStorage[tabMediaKey].some((e) => e.url === iframeBlobUrl), "frameUnloaded must prune dead blob: captures from unloaded iframe");
  assert.ok(testStorage[tabMediaKey].some((e) => e.url === iframeHlsUrl), "frameUnloaded must preserve valid HTTP(S) streams from unloaded iframe");

  // 8. ServiceWorker Stream Puller end-to-end orchestration in background worker
  const swUrl = "https://mega.example.test/sw-stream/file-1";
  const swSourceBytes = Buffer.alloc(70000);
  for (let i = 0; i < swSourceBytes.length; i++) swSourceBytes[i] = (i * 19 + 7) & 0xFF;
  const swChunkBuffers = [
    swSourceBytes.subarray(0, 32768),
    swSourceBytes.subarray(32768, 65536),
    swSourceBytes.subarray(65536)
  ];
  let swReleased = false;
  mock.nativeMessages.length = 0;
  mock.calls.length = 0;
  mock.api.tabs.sendMessage = async (_tabId, msg) => {
    if (msg.type === "swCheckScope") {
      return { ok: true, controlled: msg.url === swUrl, scope: "https://mega.example.test/" };
    }
    if (msg.type === "swPrepare") {
      return {
        ok: true,
        streamId: msg.streamId,
        url: swUrl,
        mime: "application/zip",
        disposition: 'attachment; filename="MegaDecrypted.zip"',
        length: swSourceBytes.length
      };
    }
    if (msg.type === "swPull") {
      if (msg.sequence < swChunkBuffers.length) {
        const buf = swChunkBuffers[msg.sequence];
        return { ok: true, streamId: msg.streamId, sequence: msg.sequence, end: false, data: buf.toString("base64"), bytes: buf.length };
      }
      return { ok: true, streamId: msg.streamId, sequence: msg.sequence, end: true, data: "", bytes: 0 };
    }
    if (msg.type === "swRelease") {
      swReleased = true;
      return { ok: true };
    }
    return { ok: false };
  };

  await context.redirectBrowserDownload(
    { id: 501, tabId: 101, url: swUrl, filename: "", mime: "application/zip" },
    swUrl
  );
  const swNativeTypes = mock.nativeMessages.map((m) => m.type);
  assert.deepEqual(
    swNativeTypes,
    ["swStreamBegin", "swStreamChunk", "swStreamChunk", "swStreamChunk", "swStreamFinish"],
    `${rootName} must stream ServiceWorker-controlled downloads in 32 KiB chunks over Native Messaging`
  );
  assert.equal(mock.nativeMessages[0].filename, "MegaDecrypted.zip");
  assert.equal(mock.nativeMessages[0].expectedSize, 70000);
  assert.equal(mock.nativeMessages[4].totalBytes, 70000);
  const reassembled = Buffer.concat(
    mock.nativeMessages
      .filter((m) => m.type === "swStreamChunk")
      .map((m) => Buffer.from(m.data, "base64"))
  );
  assert.ok(reassembled.equals(swSourceBytes), "Reassembled ServiceWorker stream chunks must match original bytes exactly");

  // Mid-stream failure must send BOTH swRelease to tab AND swStreamAbort to native host
  swReleased = false;
  mock.nativeMessages.length = 0;
  const origSendNative = mock.api.runtime.sendNativeMessage;
  mock.api.runtime.sendNativeMessage = async (host, msg) => {
    if (msg.type === "swStreamChunk" && msg.sequence === 1) {
      mock.nativeMessages.push(msg);
      return { ok: false, message: "disk full" };
    }
    return origSendNative(host, msg);
  };
  await assert.rejects(
    context.pullServiceWorkerStream(101, swUrl, "FailTest.zip"),
    /disk full/
  );
  assert.equal(swReleased, true, "Mid-stream failure must release tab ReadableStream reader");
  assert.equal(mock.nativeMessages.at(-1)?.type, "swStreamAbort", "Mid-stream failure must abort native host stream session");
  mock.api.runtime.sendNativeMessage = origSendNative;

  // Selected links batch dispatch & empty selection error handling
  mock.nativeMessages.length = 0;
  for (const listener of mock.api.contextMenus.onClicked.listeners) {
    listener(
      { menuItemId: "qtidm-selected-links", frameId: 0, frameUrl: "https://example.test/frame" },
      { id: 7, url: "https://example.test/page" }
    );
  }
  await new Promise((resolve) => setTimeout(resolve, 20));
  const batchMsg = mock.nativeMessages.at(-1);
  assert.ok(Array.isArray(batchMsg?.downloads) && batchMsg.downloads.length === 2, "qtidm-selected-links must dispatch selected URLs as a batch");
  assert.equal(batchMsg.downloads[0].url, "https://example.test/a.pdf");
  assert.equal(batchMsg.downloads[1].url, "https://example.test/b.pdf");
  assert.equal(batchMsg.downloads[0].headers.Referer, "https://example.test/frame");
  await assert.rejects(
    context.sendBatchToHost([], { url: "https://example.test/page" }),
    /No downloadable HTTP, HTTPS, or FTP links were found in the selection/
  );
}

async function testContentBridge(relativePath) {
  const sentMessages = [];
  const runtimeListeners = [];
  const windowListeners = new Map();
  const docListeners = new Map();
  const clickedAnchors = [];
  let blobIdCounter = 0;
  const liveObjectUrls = new Set();

  class FakeFile {
    constructor(parts, name, opts = {}) {
      this.name = name;
      this.type = opts.type || "";
      this.size = opts.size ?? parts.reduce((sum, p) => sum + String(p).length, 0);
    }
  }
  class FakeMediaSource {}
  class FakeAnchor {
    constructor() {
      this.href = "";
      this.download = "";
      this.style = {};
      this.removed = false;
    }
    click() {
      clickedAnchors.push({ href: this.href, download: this.download, internal: !!this.__qtidmInternalDownload });
    }
    remove() { this.removed = true; }
  }

  let injectedScriptSource = "";
  const docEl = {
    appendChild(node) {
      if (node.tagName === "SCRIPT") injectedScriptSource = node.textContent;
      return node;
    }
  };

  function makeURLCtor() {
    class MockURL extends URL {}
    MockURL.createObjectURL = function(target) {
      const id = ++blobIdCounter;
      const url = `blob:https://app.example.test/uuid-${id}`;
      liveObjectUrls.add(url);
      return url;
    };
    MockURL.revokeObjectURL = function(url) {
      liveObjectUrls.delete(url);
    };
    return MockURL;
  }

  const MockURL = makeURLCtor();
  let vmGlobal = null;
  const context = {
    URL: MockURL,
    File: FakeFile,
    MediaSource: FakeMediaSource,
    HTMLAnchorElement: FakeAnchor,
    XMLHttpRequest: { prototype: { open() {} } },
    MutationObserver: class { observe() {} },
    location: { href: "https://app.example.test/workspace" },
    setTimeout(fn, ms) {
      if (ms >= 1000) return { fn, ms }; // keep long timers inactive in synchronous tests
      return global.setTimeout(fn, ms);
    },
    clearTimeout(id) { if (typeof id !== "object") global.clearTimeout(id); },
    addEventListener(type, fn) {
      if (!windowListeners.has(type)) windowListeners.set(type, []);
      windowListeners.get(type).push(fn);
    },
    postMessage(data) {
      for (const fn of windowListeners.get("message") || []) {
        fn({ source: vmGlobal, data });
      }
    },
    document: {
      documentElement: docEl,
      head: docEl,
      body: docEl,
      addEventListener(type, fn) {
        if (!docListeners.has(type)) docListeners.set(type, []);
        docListeners.get(type).push(fn);
      },
      createElement(tag) {
        const upper = String(tag).toUpperCase();
        if (upper === "A") return new FakeAnchor();
        return { tagName: upper, textContent: "", remove() {} };
      }
    },
    chrome: {
      runtime: {
        async sendMessage(msg) { sentMessages.push(msg); return { ok: true }; },
        onMessage: { addListener(fn) { runtimeListeners.push(fn); } }
      }
    },
    fetch: async () => ({})
  };
  context.window = context;
  context.globalThis = context;

  vm.createContext(context);
  vmGlobal = vm.runInContext("this", context);
  vm.runInContext(fs.readFileSync(path.join(ROOT, relativePath), "utf8"), context, { filename: relativePath });
  assert.ok(injectedScriptSource.length > 0, `${relativePath} must inject the page hook script`);
  vm.runInContext(injectedScriptSource, context, { filename: `${relativePath}#page-hook` });

  // 1. Native toString() masking
  assert.equal(context.URL.createObjectURL.toString(), "function createObjectURL() { [native code] }");
  assert.equal(context.URL.revokeObjectURL.toString(), "function revokeObjectURL() { [native code] }");
  assert.equal(context.HTMLAnchorElement.prototype.click.toString(), "function click() { [native code] }");

  // 2. Noise blobs must be ignored (Web Worker JS, CSS, SVG, tiny PNG icon)
  sentMessages.length = 0;
  context.URL.createObjectURL({ size: 8192, type: "application/javascript" });
  context.URL.createObjectURL({ size: 4096, type: "text/css" });
  context.URL.createObjectURL({ size: 2048, type: "image/svg+xml" });
  context.URL.createObjectURL({ size: 512, type: "image/png" });
  assert.equal(sentMessages.length, 0, `${relativePath} must ignore noisy JS/CSS/SVG/tiny-icon blobs`);

  // 3. Real downloadable File with MIME parameters must be captured
  const pdfFile = new FakeFile(["%PDF-1.7"], "Invoice_2026.pdf", { type: "application/pdf; charset=utf-8", size: 4096 });
  const pdfUrl = context.URL.createObjectURL(pdfFile);
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, pdfUrl);
  assert.equal(sentMessages[0].mediaType, "Document");
  assert.equal(sentMessages[0].filename, "Invoice_2026.pdf");
  assert.equal(sentMessages[0].size, 4096);

  // 4. MediaSource object URLs must NEVER be dispatched even when attached to <video>
  sentMessages.length = 0;
  const mseUrl = context.URL.createObjectURL(new FakeMediaSource());
  assert.equal(sentMessages.length, 0, "MediaSource createObjectURL must not emit downloadable media");
  assert.equal(context.__qtidm_blobs.get(mseUrl).blob, null, "MediaSource object must not be strongly retained in RAM");
  context.inspectMediaElement({ tagName: "VIDEO", currentSrc: mseUrl, src: mseUrl });
  assert.equal(sentMessages.length, 0, "inspectMediaElement must not dispatch MediaSource blob URLs as downloadable video");

  // 5. Premature revocation + refreshBlobUrl must regenerate a live URL without duplicate discovery
  context.URL.revokeObjectURL(pdfUrl);
  assert.equal(liveObjectUrls.has(pdfUrl), false);
  const bytesBeforeRefresh = context.__qtidm_blobs.stats().totalCachedBytes;
  sentMessages.length = 0;
  let refreshReply = null;
  runtimeListeners[0]({ type: "refreshBlobUrl", url: pdfUrl }, {}, (res) => { refreshReply = res; });
  assert.equal(refreshReply?.ok, true, "refreshBlobUrl must succeed for prematurely revoked cached blob");
  assert.ok(refreshReply.url && refreshReply.url !== pdfUrl && liveObjectUrls.has(refreshReply.url));
  assert.equal(sentMessages.length, 0, "refreshBlobUrl must not emit a duplicate discoveredMedia item");
  assert.equal(context.__qtidm_blobs.stats().totalCachedBytes, bytesBeforeRefresh, "refreshBlobUrl must not double-count cached bytes");

  // 6. Unknown URL refresh must fail immediately (not wait for timeout)
  let missingReply = null;
  runtimeListeners[0]({ type: "refreshBlobUrl", url: "blob:https://app.example.test/nonexistent" }, {}, (res) => { missingReply = res; });
  assert.equal(missingReply?.ok, false);
  assert.equal(missingReply?.url, null);

  // 7. Oversized (>100 MiB) blob must still be reported with true size without wiping existing cache
  sentMessages.length = 0;
  const hugeUrl = context.URL.createObjectURL({ size: 150 * 1024 * 1024, type: "video/mp4" });
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].size, 150 * 1024 * 1024);
  assert.equal(sentMessages[0].mediaType, "Video");
  assert.equal(context.__qtidm_blobs.get(hugeUrl).blob, null, "Oversized blob must not be retained in RAM");
  assert.ok(context.__qtidm_blobs.get(pdfUrl)?.blob, "Oversized blob must not evict existing cached blobs");

  // 8. Anchor click filename enrichment for unnamed Blob
  sentMessages.length = 0;
  const unnamedZipUrl = context.URL.createObjectURL({ size: 128 * 1024, type: "application/zip" });
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].filename, "");
  const pageAnchor = context.document.createElement("a");
  pageAnchor.href = unnamedZipUrl;
  pageAnchor.download = "ExportedData.zip";
  pageAnchor.click();
  assert.equal(sentMessages.length, 2, "Programmatic anchor click must update blob filename");
  assert.equal(sentMessages[1].filename, "ExportedData.zip");

  // 9. downloadBlobInPage must refresh revoked blob and click a hidden anchor
  clickedAnchors.length = 0;
  let inPageReply = null;
  runtimeListeners[0]({ type: "downloadBlobInPage", url: pdfUrl, filename: "Invoice_2026.pdf" }, {}, (res) => { inPageReply = res; });
  assert.equal(inPageReply?.ok, true);
  assert.equal(clickedAnchors.length, 1);
  assert.equal(clickedAnchors[0].download, "Invoice_2026.pdf");
  assert.ok(liveObjectUrls.has(clickedAnchors[0].href), "downloadBlobInPage must use a live refreshed URL for revoked blobs");
}

async function testShadowDomTraversal(relativePath) {
  function createMockDomEnv({ allowInlineScript = true, preExistingOpenShadowUrl = "" } = {}) {
    const sentMessages = [];
    const windowListeners = new Map();
    const docListeners = new Map();
    const observers = [];
    let injectedScriptSource = "";
    let blobSeq = 0;

    function notifyObservers(target, mutation) {
      for (const obs of observers) {
        if (obs.targets.has(target)) {
          obs.callback([mutation]);
        }
      }
    }

    function matchSimpleSelector(node, selector) {
      if (!node || node.nodeType !== 1) return false;
      const parts = selector.split(",").map((s) => s.trim().toLowerCase());
      const tag = String(node.tagName || "").toLowerCase();
      for (const part of parts) {
        if (part === "*") return true;
        if (part === tag) return true;
        if (part === "a[href^='qtidm:']" && tag === "a" && String(node.href || "").startsWith("qtidm:")) return true;
        if (part === "a[href^='blob:'][download]" && tag === "a" && String(node.href || "").startsWith("blob:") && Boolean(node.download || node.getAttribute("download"))) return true;
      }
      return false;
    }

    class MockNode {
      constructor(tagName = "", nodeType = 1) {
        this.tagName = tagName.toUpperCase();
        this.nodeType = nodeType;
        this.children = [];
        this.parentElement = null;
        this.rootRef = null;
        this.shadowRoot = null;
        this.attributes = new Map();
        this.listeners = new Map();
        this.isConnected = false;
      }
      setAttribute(name, val) {
        const lower = String(name).toLowerCase();
        this.attributes.set(lower, String(val));
        if (lower === "src") this.src = String(val);
        if (lower === "href") this.href = String(val);
        if (lower === "download") this.download = String(val);
        const root = this.getRootNode();
        notifyObservers(root, { type: "attributes", attributeName: lower, target: this });
      }
      getAttribute(name) {
        return this.attributes.get(String(name).toLowerCase()) ?? null;
      }
      removeAttribute(name) {
        this.attributes.delete(String(name).toLowerCase());
      }
      getRootNode() {
        let cur = this;
        while (cur.parentElement) cur = cur.parentElement;
        return cur.rootRef || cur;
      }
      appendChild(child) {
        if (child.tagName === "SCRIPT") {
          injectedScriptSource = child.textContent;
          if (allowInlineScript && injectedScriptSource) {
            vm.runInContext(injectedScriptSource, context, { filename: `${relativePath}#page-hook` });
          }
          return child;
        }
        child.parentElement = this.nodeType === 1 ? this : null;
        child.rootRef = this.nodeType === 11 ? this : this.getRootNode();
        child.isConnected = this.isConnected || this.nodeType === 11;
        this.children.push(child);
        const root = this.nodeType === 11 ? this : this.getRootNode();
        notifyObservers(root, { type: "childList", addedNodes: [child], target: this });
        return child;
      }
      addEventListener(type, fn) {
        if (!this.listeners.has(type)) this.listeners.set(type, []);
        this.listeners.get(type).push(fn);
      }
      dispatchCaptureEvent(type, targetEl, extra = {}) {
        const eventObj = {
          type,
          target: targetEl,
          preventDefault() { this.defaultPrevented = true; },
          stopImmediatePropagation() { this.stopped = true; },
          ...extra
        };
        for (const fn of this.listeners.get(type) || []) {
          fn(eventObj);
        }
        return eventObj;
      }
      matches(selector) {
        return matchSimpleSelector(this, selector);
      }
      closest(selector) {
        let cur = this;
        while (cur) {
          if (cur.nodeType === 1 && matchSimpleSelector(cur, selector)) return cur;
          cur = cur.parentElement;
        }
        return null;
      }
      querySelectorAll(selector) {
        const results = [];
        const walk = (n) => {
          for (const child of n.children) {
            if (matchSimpleSelector(child, selector)) results.push(child);
            walk(child);
          }
        };
        walk(this);
        return results;
      }
      remove() {}
    }

    class MockShadowRoot extends MockNode {
      constructor(host, mode) {
        super("#shadow-root", 11);
        this.host = host;
        this.mode = mode;
      }
    }

    class MockElement extends MockNode {
      attachShadow(init) {
        if (!init || (init.mode !== "open" && init.mode !== "closed")) {
          throw new TypeError("Invalid shadow mode");
        }
        const sr = new MockShadowRoot(this, init.mode);
        if (init.mode === "open") this.shadowRoot = sr;
        return sr;
      }
    }

    class MockMutationObserver {
      constructor(callback) {
        this.callback = callback;
        this.targets = new Set();
        observers.push(this);
      }
      observe(target) {
        this.targets.add(target);
      }
      disconnect() {
        this.targets.clear();
      }
    }

    class MockMediaSource {}
    class MockAnchor extends MockElement {
      constructor() { super("A", 1); this.href = ""; this.download = ""; this.style = {}; }
      click() {}
    }

    class MockURL extends URL {}
    MockURL.createObjectURL = function() {
      return `blob:https://shadow.example.test/blob-${++blobSeq}`;
    };
    MockURL.revokeObjectURL = function() {};

    const docElement = new MockElement("HTML", 1);
    docElement.isConnected = true;

    if (preExistingOpenShadowUrl) {
      const host = new MockElement("PRE-EXISTING-PLAYER", 1);
      const sr = new MockShadowRoot(host, "open");
      host.shadowRoot = sr;
      const vid = new MockElement("VIDEO", 1);
      vid.src = preExistingOpenShadowUrl;
      sr.children.push(vid);
      vid.rootRef = sr;
      docElement.children.push(host);
      host.parentElement = docElement;
      host.isConnected = true;
    }

    let vmGlobal = null;
    const context = {
      URL: MockURL,
      Element: MockElement,
      MediaSource: MockMediaSource,
      HTMLAnchorElement: MockAnchor,
      XMLHttpRequest: { prototype: { open() {} } },
      MutationObserver: MockMutationObserver,
      location: { href: "https://shadow.example.test/watch" },
      setTimeout(fn, ms) {
        if (ms >= 1000) return { fn, ms };
        return global.setTimeout(fn, ms);
      },
      clearTimeout(id) { if (typeof id !== "object") global.clearTimeout(id); },
      addEventListener(type, fn) {
        if (!windowListeners.has(type)) windowListeners.set(type, []);
        windowListeners.get(type).push(fn);
      },
      postMessage(data) {
        for (const fn of windowListeners.get("message") || []) {
          fn({ source: vmGlobal, data });
        }
      },
      document: {
        readyState: "complete",
        documentElement: docElement,
        head: docElement,
        body: docElement,
        addEventListener(type, fn) {
          if (!docListeners.has(type)) docListeners.set(type, []);
          docListeners.get(type).push(fn);
        },
        createElement(tag) {
          const upper = String(tag).toUpperCase();
          if (upper === "A") return new MockAnchor();
          return new MockElement(upper, 1);
        }
      },
      chrome: {
        runtime: {
          async sendMessage(msg) { sentMessages.push(msg); return { ok: true }; },
          onMessage: { addListener() {} }
        }
      },
      fetch: async () => ({})
    };
    context.window = context;
    context.globalThis = context;
    vm.createContext(context);
    vmGlobal = vm.runInContext("this", context);
    vm.runInContext(fs.readFileSync(path.join(ROOT, relativePath), "utf8"), context, { filename: relativePath });
    return { context, sentMessages, windowListeners, docElement, MockElement, MockShadowRoot, MockAnchor, MockMediaSource };
  }

  // --- A. Full Main-World Page Hook Shadow DOM Tests ---
  const env = createMockDomEnv({
    allowInlineScript: true,
    preExistingOpenShadowUrl: "https://cdn.example.test/preexisting-dsd.mp4"
  });
  const { context, sentMessages, windowListeners, docElement, MockElement, MockShadowRoot, MockMediaSource } = env;

  // 1. Pre-existing open Shadow DOM discovered EXACTLY ONCE (zero duplicate observers) & DOM attribute cleaned up
  assert.equal(docElement.getAttribute("data-qtidm-hook"), null, "Temporary handshake attribute must be removed from DOM");
  assert.equal(sentMessages.length, 1, `${relativePath} must report pre-existing open Shadow DOM media exactly once without duplication`);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/preexisting-dsd.mp4");
  assert.equal(sentMessages[0].mediaType, "Video");

  // 2. Native toString() masking on Element.prototype.attachShadow
  assert.equal(context.Element.prototype.attachShadow.toString(), "function attachShadow() { [native code] }");

  // 3. Open Shadow DOM on detached custom element (attached in constructor before connecting)
  sentMessages.length = 0;
  const openHost = new MockElement("YTD-PLAYER", 1);
  assert.equal(openHost.isConnected, false);
  const openShadow = openHost.attachShadow({ mode: "open" });
  assert.equal(context.__qtidm_shadow.isObserved(openShadow), true, "Detached open shadow root must be enrolled immediately");
  const countAfterOpen = context.__qtidm_shadow.stats().enrolledCount;
  assert.equal(context.__qtidm_shadow.enroll(openShadow), false, "Re-enrolling same shadow root must be idempotent");
  assert.equal(context.__qtidm_shadow.stats().enrolledCount, countAfterOpen);

  // Dynamically add <video> inside open Shadow DOM
  const shadowVideo = new MockElement("VIDEO", 1);
  shadowVideo.src = "https://cdn.example.test/shadow-video.mp4";
  openShadow.appendChild(shadowVideo);
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/shadow-video.mp4");
  assert.equal(sentMessages[0].mediaType, "Video");

  // Mutating src attribute to .m3u8 must classify as HLS (not downgraded to Video)
  sentMessages.length = 0;
  shadowVideo.setAttribute("src", "https://cdn.example.test/live/master.m3u8");
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/live/master.m3u8");
  assert.equal(sentMessages[0].mediaType, "HLS");

  // Mutating src attribute on <img> inside Shadow DOM must be ignored
  sentMessages.length = 0;
  const shadowImg = new MockElement("IMG", 1);
  openShadow.appendChild(shadowImg);
  shadowImg.setAttribute("src", "https://cdn.example.test/poster.jpg");
  assert.equal(sentMessages.length, 0, "<img> src mutations inside Shadow DOM must never emit media discoveries");

  // <source> appended inside <audio> in Shadow DOM
  sentMessages.length = 0;
  const shadowAudio = new MockElement("AUDIO", 1);
  openShadow.appendChild(shadowAudio);
  const audioSource = new MockElement("SOURCE", 1);
  audioSource.src = "https://cdn.example.test/podcast.flac";
  shadowAudio.appendChild(audioSource);
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/podcast.flac");
  assert.equal(sentMessages[0].mediaType, "Audio");

  // 4. Closed Shadow DOM: preserves host.shadowRoot === null AND captures playback events
  sentMessages.length = 0;
  const closedHost = new MockElement("DRM-PLAYER", 1);
  const closedShadow = closedHost.attachShadow({ mode: "closed" });
  assert.equal(closedHost.shadowRoot, null, "Closed shadow root encapsulation must not be violated");
  assert.equal(context.__qtidm_shadow.isObserved(closedShadow), true, "Closed shadow root must be enrolled");
  const closedVideo = new MockElement("VIDEO", 1);
  closedVideo.currentSrc = "https://cdn.example.test/closed-stream.webm";
  closedShadow.dispatchCaptureEvent("play", closedVideo);
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/closed-stream.webm");
  assert.equal(sentMessages[0].mediaType, "Video");

  // Closed Shadow DOM click on <a href="blob:..." download="..."> enriches filename
  sentMessages.length = 0;
  const closedBlobUrl = context.URL.createObjectURL({ size: 256 * 1024, type: "application/pdf" });
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].filename, "");
  const closedAnchor = new MockElement("A", 1);
  closedAnchor.href = closedBlobUrl;
  closedAnchor.setAttribute("download", "ClosedShadowReport.pdf");
  closedShadow.dispatchCaptureEvent("click", closedAnchor);
  assert.equal(sentMessages.length, 2, "Click inside closed Shadow DOM must enrich blob download filename");
  assert.equal(sentMessages[1].filename, "ClosedShadowReport.pdf");

  // 5. Nested Shadow DOMs (3 levels deep: open -> closed -> open)
  sentMessages.length = 0;
  const level1 = new MockElement("APP-SHELL", 1);
  const sr1 = level1.attachShadow({ mode: "open" });
  const level2 = new MockElement("MEDIA-CONTAINER", 1);
  sr1.appendChild(level2);
  const sr2 = level2.attachShadow({ mode: "closed" });
  const level3 = new MockElement("VIDEO-VIEWPORT", 1);
  sr2.appendChild(level3);
  const sr3 = level3.attachShadow({ mode: "open" });
  const nestedVideo = new MockElement("VIDEO", 1);
  nestedVideo.src = "https://cdn.example.test/nested-3-levels.mp4";
  sr3.appendChild(nestedVideo);
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/nested-3-levels.mp4");
  assert.equal(sentMessages[0].mediaType, "Video");

  // 6. MediaSource blob inside Shadow DOM is dropped; real video Blob inside Shadow DOM is captured
  sentMessages.length = 0;
  const mseBlobUrl = context.URL.createObjectURL(new MockMediaSource());
  const mseShadowVideo = new MockElement("VIDEO", 1);
  mseShadowVideo.src = mseBlobUrl;
  openShadow.appendChild(mseShadowVideo);
  assert.equal(sentMessages.length, 0, "MediaSource blob inside Shadow DOM must not be emitted as downloadable media");

  const realBlobUrl = context.URL.createObjectURL({ size: 2 * 1024 * 1024, type: "video/mp4" });
  sentMessages.length = 0;
  const realBlobVideo = new MockElement("VIDEO", 1);
  realBlobVideo.src = realBlobUrl;
  openShadow.appendChild(realBlobVideo);
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, realBlobUrl);
  assert.equal(sentMessages[0].size, 2 * 1024 * 1024);

  // 7. composedPath() click handling for qtidm: link inside open Shadow DOM
  sentMessages.length = 0;
  const qtidmAnchor = new MockElement("A", 1);
  qtidmAnchor.href = "qtidm:https://cdn.example.test/bundle.tar.gz";
  const innerSpan = new MockElement("SPAN", 1);
  qtidmAnchor.appendChild(innerSpan);
  const clickEvt = {
    target: openHost, // retargeted to shadow host on window
    composedPath: () => [innerSpan, qtidmAnchor, openShadow, openHost],
    defaultPrevented: false,
    preventDefault() { this.defaultPrevented = true; },
    stopImmediatePropagation() {}
  };
  for (const fn of windowListeners.get("click") || []) fn(clickEvt);
  assert.equal(clickEvt.defaultPrevented, true, "qtidm: link inside open Shadow DOM must prevent default navigation");
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].type, "qtidmScheme");
  assert.equal(sentMessages[0].url, "https://cdn.example.test/bundle.tar.gz");

  // 8. Dynamically inserted Declarative Shadow DOM in Light DOM (no attachShadow call)
  sentMessages.length = 0;
  const dsdHost = new MockElement("DECLARATIVE-PLAYER", 1);
  const dsdRoot = new MockShadowRoot(dsdHost, "open");
  dsdHost.shadowRoot = dsdRoot;
  const dsdVideo = new MockElement("VIDEO", 1);
  dsdVideo.src = "https://cdn.example.test/dynamic-dsd.mp4";
  dsdRoot.children.push(dsdVideo);
  dsdVideo.rootRef = dsdRoot;
  docElement.appendChild(dsdHost);
  assert.equal(sentMessages.length, 1, "Dynamically appended Declarative Shadow DOM host must be discovered via scanShadows");
  assert.equal(sentMessages[0].url, "https://cdn.example.test/dynamic-dsd.mp4");

  // 9. Depth cap guard (15-level chain must terminate cleanly without error or runaway recursion)
  let currentHost = new MockElement("DEEP-0", 1);
  let deepRoot = new MockShadowRoot(currentHost, "open");
  currentHost.shadowRoot = deepRoot;
  for (let d = 1; d <= 15; d++) {
    const child = new MockElement(`DEEP-${d}`, 1);
    const childRoot = new MockShadowRoot(child, "open");
    child.shadowRoot = childRoot;
    deepRoot.children.push(child);
    deepRoot = childRoot;
  }
  assert.doesNotThrow(() => context.__qtidm_shadow.scan(currentHost), "Deeply nested shadow tree must respect MAX_SHADOW_DEPTH cleanly");

  // --- B. Strict-CSP Fallback Mode (inline script blocked) ---
  const cspEnv = createMockDomEnv({
    allowInlineScript: false,
    preExistingOpenShadowUrl: "https://cdn.example.test/csp-open-shadow.mp4"
  });
  assert.ok(
    cspEnv.sentMessages.some((m) => m.type === "discoveredMedia" && m.url === "https://cdn.example.test/csp-open-shadow.mp4"),
    `${relativePath} must fall back to isolated-world open Shadow DOM traversal when CSP blocks inline scripts`
  );
  // Dynamically added host with nested open shadow root under strict CSP
  cspEnv.sentMessages.length = 0;
  const cspHost = new cspEnv.MockElement("CSP-PLAYER", 1);
  const cspRoot = new cspEnv.MockShadowRoot(cspHost, "open");
  cspHost.shadowRoot = cspRoot;
  const cspAudio = new cspEnv.MockElement("AUDIO", 1);
  cspAudio.src = "https://cdn.example.test/csp-nested-audio.mp3";
  cspRoot.children.push(cspAudio);
  cspAudio.rootRef = cspRoot;
  cspEnv.docElement.appendChild(cspHost);
  assert.equal(cspEnv.sentMessages.length, 1);
  assert.equal(cspEnv.sentMessages[0].url, "https://cdn.example.test/csp-nested-audio.mp3");
  assert.equal(cspEnv.sentMessages[0].mediaType, "Audio");
}

async function testFrameworkComponentSniffing(relativePath) {
  const sentMessages = [];
  const windowListeners = new Map();
  const docListeners = new Map();
  const observers = [];
  let injectedScriptSource = "";
  let blobSeq = 0;

  function notifyObservers(target, mutation) {
    for (const obs of observers) {
      if (obs.targets.has(target)) obs.callback([mutation]);
    }
  }

  class MockNode {
    constructor(tagName = "", nodeType = 1) {
      this.tagName = tagName.toUpperCase();
      this.nodeType = nodeType;
      this.children = [];
      this.parentElement = null;
      this.parentNode = null;
      this.rootRef = null;
      this.shadowRoot = null;
      this.attributes = new Map();
      this.dataset = {};
      this.listeners = new Map();
    }
    setAttribute(name, val) {
      const lower = String(name).toLowerCase();
      this.attributes.set(lower, String(val));
      if (lower === "src") this.src = String(val);
      notifyObservers(this.getRootNode(), { type: "attributes", attributeName: lower, target: this });
    }
    getAttribute(name) { return this.attributes.get(String(name).toLowerCase()) ?? null; }
    removeAttribute(name) { this.attributes.delete(String(name).toLowerCase()); }
    getRootNode() {
      let cur = this;
      while (cur.parentElement) cur = cur.parentElement;
      return cur.rootRef || cur;
    }
    appendChild(child) {
      if (child.tagName === "SCRIPT") {
        injectedScriptSource = child.textContent;
        vm.runInContext(injectedScriptSource, context, { filename: `${relativePath}#page-hook` });
        return child;
      }
      child.parentElement = this.nodeType === 1 ? this : null;
      child.parentNode = this;
      child.rootRef = this.nodeType === 11 ? this : this.getRootNode();
      this.children.push(child);
      notifyObservers(this.nodeType === 11 ? this : this.getRootNode(), { type: "childList", addedNodes: [child], target: this });
      return child;
    }
    addEventListener(type, fn) {
      if (!this.listeners.has(type)) this.listeners.set(type, []);
      this.listeners.get(type).push(fn);
    }
    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
    querySelectorAll(selector) {
      const parts = selector.split(",").map((s) => s.trim().toLowerCase());
      const results = [];
      const walk = (n) => {
        for (const c of n.children) {
          const tag = String(c.tagName || "").toLowerCase();
          if (parts.includes("*") || parts.includes(tag)) results.push(c);
          walk(c);
        }
      };
      walk(this);
      return results;
    }
    remove() {}
  }

  class MockShadowRoot extends MockNode {
    constructor(host, mode) { super("#shadow-root", 11); this.host = host; this.mode = mode; }
  }
  class MockElement extends MockNode {
    attachShadow(init) {
      const sr = new MockShadowRoot(this, init.mode);
      if (init.mode === "open") this.shadowRoot = sr;
      return sr;
    }
  }
  class MockMutationObserver {
    constructor(cb) { this.callback = cb; this.targets = new Set(); observers.push(this); }
    observe(t) { this.targets.add(t); }
  }
  class MockMediaSource {}
  class MockURL extends URL {}
  MockURL.createObjectURL = () => `blob:https://spa.example.test/mse-${++blobSeq}`;
  MockURL.revokeObjectURL = () => {};

  const docElement = new MockElement("HTML", 1);
  let vmGlobal = null;
  const context = {
    URL: MockURL,
    Element: MockElement,
    MediaSource: MockMediaSource,
    HTMLAnchorElement: { prototype: { click() {} } },
    XMLHttpRequest: { prototype: { open() {} } },
    MutationObserver: MockMutationObserver,
    location: { href: "https://spa.example.test/watch/episode-1" },
    setTimeout(fn, ms) { return ms >= 1000 ? { fn, ms } : global.setTimeout(fn, ms); },
    clearTimeout(id) { if (typeof id !== "object") global.clearTimeout(id); },
    addEventListener(type, fn) {
      if (!windowListeners.has(type)) windowListeners.set(type, []);
      windowListeners.get(type).push(fn);
    },
    postMessage(data) {
      for (const fn of windowListeners.get("message") || []) fn({ source: vmGlobal, data });
    },
    document: {
      readyState: "complete",
      documentElement: docElement,
      head: docElement,
      body: docElement,
      addEventListener(type, fn) {
        if (!docListeners.has(type)) docListeners.set(type, []);
        docListeners.get(type).push(fn);
      },
      createElement(tag) { return new MockElement(String(tag).toUpperCase(), 1); }
    },
    chrome: {
      runtime: {
        async sendMessage(msg) { sentMessages.push(msg); return { ok: true }; },
        onMessage: { addListener() {} }
      }
    },
    fetch: async () => ({})
  };
  context.window = context;
  context.globalThis = context;
  vm.createContext(context);
  vmGlobal = vm.runInContext("this", context);
  vm.runInContext(fs.readFileSync(path.join(ROOT, relativePath), "utf8"), context, { filename: relativePath });

  const sniffer = context.__qtidm_component_sniffer;
  assert.ok(sniffer && typeof sniffer.sniffElement === "function", `${relativePath} must install window.__qtidm_component_sniffer`);

  // 1. React 18 Fiber tree (.return upward walk + Hooks linked list + noise filtering + per-element dedup)
  sentMessages.length = 0;
  const mseUrl = context.URL.createObjectURL(new MockMediaSource());
  const reactWrapper = new MockElement("DIV", 1);
  const reactVideo = new MockElement("VIDEO", 1);
  reactVideo.src = mseUrl;
  const parentFiber = {
    memoizedProps: {
      hlsUrl: "https://cdn.example.test/react/master.m3u8",
      posterUrl: "https://cdn.example.test/poster.jpg",
      thumbnailUrl: "https://cdn.example.test/thumb.webp",
      analyticsUrl: "https://metrics.example.test/ping"
    },
    memoizedState: {
      memoizedState: "unrelated-ui-state",
      next: {
        memoizedState: 42,
        next: {
          memoizedState: "https://cdn.example.test/react/fallback.mpd",
          next: null
        }
      }
    },
    return: null
  };
  const leafFiber = { memoizedProps: { autoPlay: true }, return: parentFiber };
  reactVideo["__reactFiber$abc123"] = leafFiber;
  reactWrapper.appendChild(reactVideo);
  docElement.appendChild(reactWrapper);

  assert.equal(sentMessages.length, 2, "React Fiber sniffer must unmask both HLS and DASH while ignoring MSE blob, poster, thumbnail, analytics");
  assert.equal(sentMessages[0].url, "https://cdn.example.test/react/master.m3u8");
  assert.equal(sentMessages[0].mediaType, "HLS");
  assert.equal(sentMessages[1].url, "https://cdn.example.test/react/fallback.mpd");
  assert.equal(sentMessages[1].mediaType, "DASH");

  // Firing play with unchanged state must NOT duplicate; updating state for Episode 2 MUST emit immediately
  sentMessages.length = 0;
  for (const fn of docListeners.get("play") || []) fn({ target: reactVideo });
  assert.equal(sentMessages.length, 0, "Repeated play event with unchanged component state must not re-emit duplicates");
  parentFiber.memoizedProps.hlsUrl = "https://cdn.example.test/react/ep2.m3u8";
  for (const fn of docListeners.get("play") || []) fn({ target: reactVideo });
  assert.equal(sentMessages.length, 1);
  assert.equal(sentMessages[0].url, "https://cdn.example.test/react/ep2.m3u8");

  // 2. Vue 2 ($data sources with extensionless URLs typed via sibling MIME) & Vue 3 (prototype-getter RefImpl)
  const vue2El = new MockElement("VIDEO", 1);
  vue2El.__vue__ = {
    $data: {
      sources: [
        { src: "https://cdn.example.test/vue2/stream?id=10", type: "application/x-mpegurl" },
        { src: "https://cdn.example.test/vue2/stream?id=11", type: "video/mp4" }
      ]
    }
  };
  const vue2Results = sniffer.sniffElement(vue2El);
  assert.equal(vue2Results.length, 2);
  assert.equal(vue2Results[0].url, "https://cdn.example.test/vue2/stream?id=10");
  assert.equal(vue2Results[0].mediaType, "HLS");
  assert.equal(vue2Results[1].url, "https://cdn.example.test/vue2/stream?id=11");
  assert.equal(vue2Results[1].mediaType, "Video");

  class FakeVueRef {
    constructor(v) { this._value = v; }
    get value() { return this._value; }
  }
  const vue3El = new MockElement("VIDEO", 1);
  vue3El.__vueParentComponent = {
    setupState: {
      hlsUrl: new FakeVueRef("https://cdn.example.test/vue3/api/hls?sig=xyz")
    }
  };
  const vue3Results = sniffer.sniffElement(vue3El);
  assert.equal(vue3Results.length, 1);
  assert.equal(vue3Results[0].url, "https://cdn.example.test/vue3/api/hls?sig=xyz");
  assert.equal(vue3Results[0].mediaType, "HLS");

  // 3. Preact (__k with relative URL), Svelte (__svelte.$$.ctx array), Angular Ivy (__ngContext__ LView)
  const preactEl = new MockElement("VIDEO", 1);
  preactEl.__k = { __c: { state: { streamUrl: "/streams/ep05/index.m3u8" } } };
  const preactResults = sniffer.sniffElement(preactEl);
  assert.equal(preactResults.length, 1);
  assert.equal(preactResults[0].url, "https://spa.example.test/streams/ep05/index.m3u8");
  assert.equal(preactResults[0].mediaType, "HLS");

  const svelteEl = new MockElement("VIDEO", 1);
  svelteEl.__svelte = {
    $$: {
      ctx: ["Episode Title", 123, "https://cdn.example.test/svelte/clip.webm", "https://cdn.example.test/avatar.png"]
    }
  };
  const svelteResults = sniffer.sniffElement(svelteEl);
  assert.equal(svelteResults.length, 1);
  assert.equal(svelteResults[0].url, "https://cdn.example.test/svelte/clip.webm");
  assert.equal(svelteResults[0].mediaType, "Video");

  const angularEl = new MockElement("VIDEO", 1);
  angularEl.__ngContext__ = [
    null,
    { playbackUrl: "https://cdn.example.test/angular/episode.mp4", thumbnailUrl: "https://cdn.example.test/thumb.webp" }
  ];
  const angularResults = sniffer.sniffElement(angularEl);
  assert.equal(angularResults.length, 1);
  assert.equal(angularResults[0].url, "https://cdn.example.test/angular/episode.mp4");
  assert.equal(angularResults[0].mediaType, "Video");

  // 4. Shadow DOM boundary crossing to outer host (__reactProps$, dataset, hls.js SDK ref)
  sentMessages.length = 0;
  const shadowHost = new MockElement("CUSTOM-PLAYER", 1);
  shadowHost["__reactProps$shadow"] = { dashUrl: "https://cdn.example.test/shadow-host/manifest.mpd" };
  shadowHost.dataset.hlsUrl = "https://cdn.example.test/shadow-host/master.m3u8";
  const sr = shadowHost.attachShadow({ mode: "open" });
  const innerVideo = new MockElement("VIDEO", 1);
  innerVideo.hls = { url: "https://cdn.example.test/hlsjs/live.m3u8" };
  sr.appendChild(innerVideo);
  assert.ok(sentMessages.some((m) => m.url === "https://cdn.example.test/shadow-host/master.m3u8" && m.mediaType === "HLS"));
  assert.ok(sentMessages.some((m) => m.url === "https://cdn.example.test/hlsjs/live.m3u8" && m.mediaType === "HLS"));
  assert.ok(sentMessages.some((m) => m.url === "https://cdn.example.test/shadow-host/manifest.mpd" && m.mediaType === "DASH"));

  // 5. Adversarial safety: cyclic graph, throwing getter, SSRF, credentials, root homepage, non-HTTP schemes
  const hostileObj = {
    badJs: "javascript:alert(1)",
    badData: "data:video/mp4;base64,AAAA",
    videoUrl: "https://example.com/",
    streamUrl: "http://169.254.169.254/latest/meta-data/video.mp4",
    playbackUrl: "https://user:pass@cdn.example.test/secret.mp4",
    mp4Url: "https://cdn.example.test/safe-video.mp4"
  };
  hostileObj.self = hostileObj;
  hostileObj.return = hostileObj;
  hostileObj.memoizedState = hostileObj;
  Object.defineProperty(hostileObj, "hlsUrl", {
    enumerable: true,
    get() { throw new Error("Getter threw intentionally"); }
  });
  const safeResults = sniffer.sniffObject(hostileObj);
  assert.equal(safeResults.length, 1, "Sniffer must survive cycles and throwing getters while blocking SSRF/credentials/homepage");
  assert.equal(safeResults[0].url, "https://cdn.example.test/safe-video.mp4");

  // 6. Regression check: fetch & XMLHttpRequest manifest regexes match real .m3u8 / .mpd URLs
  sentMessages.length = 0;
  await context.fetch("https://cdn.example.test/fetch/live.m3u8?token=1");
  context.XMLHttpRequest.prototype.open("GET", "https://cdn.example.test/xhr/manifest.mpd");
  assert.equal(sentMessages.length, 2, "fetch and XHR page hooks must detect .m3u8 and .mpd URLs");
  assert.equal(sentMessages[0].mediaType, "HLS");
  assert.equal(sentMessages[1].mediaType, "DASH");
}

async function testCrossOriginIframeBridge(relativePath) {
  // 1. Verify both Chrome and Firefox manifests enable all_frames + match_about_blank
  const chromeManifest = JSON.parse(fs.readFileSync(path.join(ROOT, "browser/chrome/manifest.json"), "utf8"));
  const firefoxManifest = JSON.parse(fs.readFileSync(path.join(ROOT, "browser/firefox/manifest.json"), "utf8"));
  assert.equal(chromeManifest.content_scripts[0].all_frames, true);
  assert.equal(chromeManifest.content_scripts[0].match_about_blank, true);
  assert.equal(chromeManifest.content_scripts[0].match_origin_as_fallback, true);
  assert.equal(firefoxManifest.content_scripts[0].all_frames, true);
  assert.equal(firefoxManifest.content_scripts[0].match_about_blank, true);

  function createFrameRealm({ href, referrer = "", title = "", topWindowRef = null, hasRuntime = true, childFrames = [] }) {
    const sentMessages = [];
    const runtimeListeners = [];
    const windowListeners = new Map();
    let vmGlobal = null;

    const docEl = {
      attributes: new Map(),
      getAttribute(k) { return this.attributes.get(k) ?? null; },
      setAttribute(k, v) { this.attributes.set(k, String(v)); },
      removeAttribute(k) { this.attributes.delete(k); },
      appendChild(node) {
        if (node.tagName === "SCRIPT" && node.textContent) {
          vm.runInContext(node.textContent, context, { filename: `${relativePath}#page-hook` });
        }
        return node;
      },
      querySelectorAll() { return []; }
    };

    const context = {
      URL,
      XMLHttpRequest: { prototype: { open() {} } },
      MutationObserver: class { observe() {} },
      location: { href },
      frames: childFrames,
      setTimeout(fn, ms) { return ms >= 1000 ? { fn, ms } : global.setTimeout(fn, ms); },
      clearTimeout(id) { if (typeof id !== "object") global.clearTimeout(id); },
      addEventListener(type, fn) {
        if (!windowListeners.has(type)) windowListeners.set(type, []);
        windowListeners.get(type).push(fn);
      },
      postMessage(data) {
        for (const fn of windowListeners.get("message") || []) {
          fn({ source: vmGlobal, origin: href.startsWith("http") ? new URL(href).origin : "null", data });
        }
      },
      deliverCrossFrameMessage(data, senderProxy, senderOrigin) {
        for (const fn of windowListeners.get("message") || []) {
          fn({ source: senderProxy, origin: senderOrigin, data });
        }
      },
      fireWindowEvent(type) {
        for (const fn of windowListeners.get(type) || []) fn({ type });
      },
      document: {
        readyState: "complete",
        referrer,
        title,
        documentElement: docEl,
        head: docEl,
        body: docEl,
        addEventListener() {},
        createElement(tag) { return { tagName: String(tag).toUpperCase(), textContent: "", remove() {} }; }
      },
      chrome: hasRuntime
        ? {
            runtime: {
              async sendMessage(msg) { sentMessages.push(msg); return { ok: true }; },
              onMessage: { addListener(fn) { runtimeListeners.push(fn); } }
            }
          }
        : {},
      fetch: async () => ({})
    };
    context.window = context;
    context.globalThis = context;
    context.top = topWindowRef || context;
    context.parent = topWindowRef || context;

    vm.createContext(context);
    vmGlobal = vm.runInContext("this", context);
    return {
      context,
      sentMessages,
      runtimeListeners,
      getProxy: () => vmGlobal,
      boot() {
        vm.runInContext(fs.readFileSync(path.join(ROOT, relativePath), "utf8"), context, { filename: relativePath });
      }
    };
  }

  // Create child frames first to ALSO test out-of-order topFrameReady replay!
  let topRealm = null;
  const topProxyForChildren = {
    postMessage(data) {
      if (topRealm) {
        const senderOrigin = data?.frameUrl && data.frameUrl.startsWith("http")
          ? new URL(data.frameUrl).origin
          : "null";
        topRealm.context.deliverCrossFrameMessage(data, { tag: "child-frame" }, senderOrigin);
      }
    }
  };

  const childA = createFrameRealm({
    href: "https://embed.player.test/e/123",
    title: "Embed Player A",
    topWindowRef: topProxyForChildren,
    hasRuntime: true
  });
  const childB = createFrameRealm({
    href: "about:srcdoc",
    referrer: "https://nested.embed.test/wrapper",
    title: "Sandboxed Frame B",
    topWindowRef: topProxyForChildren,
    hasRuntime: false // Sandboxed/restricted iframe without extApi.runtime
  });

  childA.boot();
  childB.boot();
  assert.equal(childA.context.__qtidm_frame_bridge.isTopFrame, false);
  assert.equal(childB.context.__qtidm_frame_bridge.isTopFrame, false);

  // Child A discovers an HLS stream BEFORE top window boots
  childA.context.dispatchDiscoveredMedia("https://edge.player.test/hls/master.m3u8", "HLS");
  assert.equal(childA.sentMessages.length, 1, "Child A with runtime must send discoveredMedia directly");
  assert.equal(childA.sentMessages[0].isSubframe, true);
  assert.equal(childA.sentMessages[0].frameUrl, "https://embed.player.test/e/123");
  assert.equal(childA.sentMessages[0].frameToken, childA.context.__qtidm_frame_bridge.frameToken);

  // Now Top Window boots and broadcasts topFrameReady to child frames
  topRealm = createFrameRealm({
    href: "https://main.site.test/watch",
    title: "Main Watch Page",
    hasRuntime: true,
    childFrames: [
      { postMessage(data) { childA.context.deliverCrossFrameMessage(data, topRealm.getProxy(), "https://main.site.test"); } },
      { postMessage(data) { childB.context.deliverCrossFrameMessage(data, topRealm.getProxy(), "https://main.site.test"); } }
    ]
  });
  topRealm.boot();
  assert.equal(topRealm.context.__qtidm_frame_bridge.isTopFrame, true);

  // Top window must have received Child A's replayed stream in frameRegistry WITHOUT sending duplicate runtime message
  assert.equal(topRealm.context.__qtidm_frame_bridge.stats().frameCount, 1);
  assert.equal(topRealm.context.__qtidm_frame_bridge.getAllMedia()[0].url, "https://edge.player.test/hls/master.m3u8");
  assert.equal(topRealm.sentMessages.length, 0, "Top window must not duplicate runtime IPC for child frames that relayed directly");

  // Child B (sandboxed, no runtime, about:srcdoc) discovers a video -> Top window MUST relay it to runtime with document.referrer!
  childB.context.dispatchDiscoveredMedia("https://edge.nested.test/ep1.mp4", "Video");
  assert.equal(childB.sentMessages.length, 0);
  assert.equal(topRealm.context.__qtidm_frame_bridge.stats().frameCount, 2);
  assert.equal(topRealm.sentMessages.length, 1, "Top window must relay discoveries from sandboxed child frames without runtime");
  assert.equal(topRealm.sentMessages[0].url, "https://edge.nested.test/ep1.mp4");
  assert.equal(topRealm.sentMessages[0].isSubframe, true);
  assert.equal(topRealm.sentMessages[0].referrer, "https://nested.embed.test/wrapper", "about:srcdoc iframe must fall back to document.referrer");

  // Adversarial spoofed postMessage envelopes must all be rejected
  topRealm.sentMessages.length = 0;
  const maliciousPayloads = [
    { source: "qtidm-frame-bridge", version: 99, type: "frameMedia", frameToken: "evil", relayedDirectly: false, media: { url: "https://cdn.test/v.mp4", mediaType: "Video" } },
    { source: "qtidm-frame-bridge", version: 1, type: "frameMedia", frameToken: "evil", relayedDirectly: false, media: { url: "javascript:alert(1)", mediaType: "Video" } },
    { source: "qtidm-frame-bridge", version: 1, type: "frameMedia", frameToken: "evil", relayedDirectly: false, media: { url: "http://169.254.169.254/latest/video.mp4", mediaType: "Video" } },
    { source: "qtidm-frame-bridge", version: 1, type: "frameMedia", frameToken: "evil", relayedDirectly: false, media: { url: "https://user:pass@cdn.test/video.mp4", mediaType: "Video" } }
  ];
  for (const payload of maliciousPayloads) {
    topRealm.context.deliverCrossFrameMessage(payload, { tag: "evil-iframe" }, "https://evil.test");
  }
  assert.equal(topRealm.sentMessages.length, 0, "Top frame bridge must reject invalid version, non-HTTP schemes, SSRF, and credentialed URLs");

  // Frame-scoped refreshBlobUrl must ignore requests meant for a different targetFrameToken
  let wrongFrameResponded = false;
  childA.runtimeListeners[0](
    { type: "refreshBlobUrl", url: "blob:https://other.test/1", targetFrameToken: "qtidm-frame-other" },
    {},
    () => { wrongFrameResponded = true; }
  );
  assert.equal(wrongFrameResponded, false, "Non-matching child frame must not hijack refreshBlobUrl meant for another frameToken");

  // Child A pagehide must notify top window (removing from frameRegistry) AND send frameUnloaded to runtime
  childA.sentMessages.length = 0;
  childA.context.fireWindowEvent("pagehide");
  assert.equal(topRealm.context.__qtidm_frame_bridge.stats().frameCount, 1, "Top window must prune unloaded child frame from frameRegistry");
  assert.equal(childA.sentMessages.length, 1);
  assert.equal(childA.sentMessages[0].type, "frameUnloaded");
  assert.equal(childA.sentMessages[0].frameToken, childA.context.__qtidm_frame_bridge.frameToken);
}

async function testServiceWorkerStreamPuller(relativePath) {
  const windowListeners = new Map();
  const runtimeListeners = [];
  let vmGlobal = null;
  const fakeController = { state: "activated" };

  // 70,000-byte deterministic buffer delivered as two unequal 50,000 + 20,000 ReadableStream chunks
  // to verify 32 KiB slicing across chunk boundaries (32768 + 32768 + 4464 = 70000).
  const sourceBytes = new Uint8Array(70000);
  for (let i = 0; i < sourceBytes.length; i++) sourceBytes[i] = (i * 41 + 13) & 0xFF;

  const docEl = {
    attributes: new Map(),
    getAttribute(k) { return this.attributes.get(k) ?? null; },
    setAttribute(k, v) { this.attributes.set(k, String(v)); },
    removeAttribute(k) { this.attributes.delete(k); },
    appendChild(node) {
      if (node.tagName === "SCRIPT" && node.textContent) {
        vm.runInContext(node.textContent, context, { filename: `${relativePath}#page-hook` });
      }
      return node;
    },
    querySelectorAll() { return []; }
  };

  let lastFetchInit = null;
  const context = {
    URL,
    AbortController,
    Uint8Array,
    btoa(binary) { return Buffer.from(binary, "binary").toString("base64"); },
    XMLHttpRequest: { prototype: { open() {} } },
    MutationObserver: class { observe() {} },
    location: { href: "https://mega.example.test/folder/1", origin: "https://mega.example.test" },
    navigator: {
      serviceWorker: {
        controller: fakeController,
        async getRegistration(url) {
          if (String(url).startsWith("https://mega.example.test/")) {
            return { active: fakeController, scope: "https://mega.example.test/" };
          }
          return null;
        }
      }
    },
    setTimeout(fn, ms) { return ms >= 1000 ? { fn, ms } : global.setTimeout(fn, ms); },
    clearTimeout(id) { if (typeof id !== "object") global.clearTimeout(id); },
    addEventListener(type, fn) {
      if (!windowListeners.has(type)) windowListeners.set(type, []);
      windowListeners.get(type).push(fn);
    },
    postMessage(data) {
      for (const fn of windowListeners.get("message") || []) {
        fn({ source: vmGlobal, origin: "https://mega.example.test", data });
      }
    },
    document: {
      readyState: "complete",
      documentElement: docEl,
      head: docEl,
      body: docEl,
      addEventListener() {},
      createElement(tag) { return { tagName: String(tag).toUpperCase(), textContent: "", remove() {} }; }
    },
    chrome: {
      runtime: {
        async sendMessage() { return { ok: true }; },
        onMessage: { addListener(fn) { runtimeListeners.push(fn); } }
      }
    },
    async fetch(url, init) {
      lastFetchInit = init;
      const chunks = [sourceBytes.subarray(0, 50000), sourceBytes.subarray(50000)];
      let idx = 0;
      return {
        status: 200,
        redirected: false,
        url,
        headers: {
          get(name) {
            const lower = String(name).toLowerCase();
            if (lower === "content-length") return String(sourceBytes.length);
            if (lower === "content-type") return "application/zip";
            if (lower === "content-disposition") return 'attachment; filename="Vault.zip"';
            return null;
          }
        },
        body: {
          getReader() {
            return {
              async read() {
                if (idx < chunks.length) return { done: false, value: chunks[idx++] };
                return { done: true, value: undefined };
              },
              async cancel() {}
            };
          }
        }
      };
    }
  };
  context.window = context;
  context.globalThis = context;
  vm.createContext(context);
  vmGlobal = vm.runInContext("this", context);
  vm.runInContext(fs.readFileSync(path.join(ROOT, relativePath), "utf8"), context, { filename: relativePath });

  const sendRuntime = (msg) => new Promise((resolve) => {
    runtimeListeners[0](msg, {}, resolve);
  });

  // 1. Scope check: same-origin controlled vs cross-origin rejected
  const sameScope = await sendRuntime({ type: "swCheckScope", url: "https://mega.example.test/sw/file-1" });
  assert.equal(sameScope.ok, true);
  assert.equal(sameScope.controlled, true);
  assert.equal(sameScope.scope, "https://mega.example.test/");

  const crossScope = await sendRuntime({ type: "swCheckScope", url: "https://other.example.test/file-1" });
  assert.equal(crossScope.controlled, false);

  // 2. Cross-origin swPrepare must be rejected with fallback: true
  const badPrep = await sendRuntime({ type: "swPrepare", streamId: "sw-cross", url: "https://other.example.test/file-1" });
  assert.equal(badPrep.ok, false);
  assert.equal(badPrep.fallback, true);

  // 3. Full prepare + 32 KiB pull loop reassembles exact 70,000-byte payload
  const prep = await sendRuntime({ type: "swPrepare", streamId: "sw-main", url: "https://mega.example.test/sw/file-1" });
  assert.equal(prep.ok, true);
  assert.equal(prep.length, 70000);
  assert.equal(prep.mime, "application/zip");
  assert.equal(lastFetchInit.credentials, "include");
  assert.equal(lastFetchInit.mode, "same-origin");
  assert.equal(lastFetchInit.redirect, "error");

  const collected = [];
  const sliceSizes = [];
  let seq = 0;
  while (true) {
    const res = await sendRuntime({ type: "swPull", streamId: "sw-main", sequence: seq });
    assert.equal(res.ok, true);
    if (res.end) break;
    sliceSizes.push(res.bytes);
    collected.push(Buffer.from(res.data, "base64"));
    seq++;
  }
  assert.deepEqual(sliceSizes, [32768, 17232, 20000], "ReadableStream chunks must be capped at 32 KiB slices without stalling across reads");
  assert.ok(sliceSizes.every((s) => s > 0 && s <= 32768));
  assert.ok(Buffer.concat(collected).equals(Buffer.from(sourceBytes)), "Pulled stream bytes must match source payload byte-for-byte");
  assert.equal(context.__qtidm_sw_puller.activeCount(), 0, "Completed stream session must be released");

  // 4. Out-of-order pull must fail and release session
  const prep2 = await sendRuntime({ type: "swPrepare", streamId: "sw-ooo", url: "https://mega.example.test/sw/file-2" });
  assert.equal(prep2.ok, true);
  const badPull = await sendRuntime({ type: "swPull", streamId: "sw-ooo", sequence: 5 });
  assert.equal(badPull.ok, false);
  assert.equal(context.__qtidm_sw_puller.activeCount(), 0);

  // 5. Concurrency limit (4) + pagehide cleanup
  for (let i = 0; i < 4; i++) {
    const p = await sendRuntime({ type: "swPrepare", streamId: `sw-cap-${i}`, url: `https://mega.example.test/sw/c-${i}` });
    assert.equal(p.ok, true);
  }
  const overflow = await sendRuntime({ type: "swPrepare", streamId: "sw-cap-5", url: "https://mega.example.test/sw/c-5" });
  assert.equal(overflow.ok, false);
  assert.equal(overflow.fallback, true);
  assert.equal(context.__qtidm_sw_puller.activeCount(), 4);

  for (const fn of windowListeners.get("pagehide") || []) fn({ type: "pagehide" });
  assert.equal(context.__qtidm_sw_puller.activeCount(), 0, "pagehide must abort and release all active ServiceWorker streams");
}

Promise.all([
  testVariant("browser/chrome/service_worker.js", "chrome"),
  testVariant("browser/firefox/background.js", "browser"),
  testContentBridge("browser/chrome/content_bridge.js"),
  testContentBridge("browser/firefox/content_bridge.js"),
  testShadowDomTraversal("browser/chrome/content_bridge.js"),
  testShadowDomTraversal("browser/firefox/content_bridge.js"),
  testFrameworkComponentSniffing("browser/chrome/content_bridge.js"),
  testFrameworkComponentSniffing("browser/firefox/content_bridge.js"),
  testCrossOriginIframeBridge("browser/chrome/content_bridge.js"),
  testCrossOriginIframeBridge("browser/firefox/content_bridge.js"),
  testServiceWorkerStreamPuller("browser/chrome/content_bridge.js"),
  testServiceWorkerStreamPuller("browser/firefox/content_bridge.js")
]).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
