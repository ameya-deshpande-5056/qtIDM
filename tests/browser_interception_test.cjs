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
      async getAll() { return []; }
    },
    tabs: {
      onRemoved: event(),
      async get() { return { url: "https://example.test/page" }; }
    },
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
}

Promise.all([
  testVariant("browser/chrome/service_worker.js", "chrome"),
  testVariant("browser/firefox/background.js", "browser")
]).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
