"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const ROOT = path.resolve(__dirname, "..");

function event() {
  const listeners = [];
  return {
    listeners,
    addListener(listener) {
      listeners.push(listener);
    }
  };
}

function browserApi(rootName) {
  const calls = [];
  const nativeMessages = [];
  let nativeError = null;
  const api = {
    runtime: {
      id: "qtidm-test-extension",
      onInstalled: event(),
      onMessage: event(),
      async sendNativeMessage(_host, message) {
        calls.push("native");
        nativeMessages.push(message);
        if (nativeError) throw nativeError;
        return { ok: true };
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
      async erase() { calls.push("erase"); }
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
    }
  };
}

async function testVariant(relativePath, rootName) {
  const mock = browserApi(rootName);
  const context = {
    [rootName]: mock.api,
    URL,
    navigator: { userAgent: "qtIDM interception test" },
    console: { error() {} }
  };
  vm.createContext(context);
  vm.runInContext(
    fs.readFileSync(path.join(ROOT, relativePath), "utf8"),
    context,
    { filename: relativePath }
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
    "HLS"
  );
  assert.equal(mock.nativeMessages.at(-1).mediaType, "HLS");
  assert.equal(mock.nativeMessages.at(-1).headers.Origin, "https://example.test");
  assert.equal(mock.nativeMessages.at(-1).headers.Referer, "https://example.test/page");
  assert.equal(mock.nativeMessages.at(-1).headers["Sec-Fetch-Site"], "same-origin");
  mock.calls.length = 0;

  const item = {
    id: 7,
    referrer: "https://example.test/page"
  };
  await context.redirectBrowserDownload(item, "https://example.test/archive.bin");
  assert.deepEqual(
    mock.calls,
    ["pause", "native", "cancel", "removeFile", "erase"],
    `${rootName} must erase a successfully redirected browser download`
  );

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
}

Promise.all([
  testVariant("browser/chrome/service_worker.js", "chrome"),
  testVariant("browser/firefox/background.js", "browser")
]).catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
