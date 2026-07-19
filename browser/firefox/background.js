const hostName = "io.github.qtidm.native";
const mediaKey = (tabId) => `qtidm-media-${tabId}`;
const settingsKey = "qtidm-settings";
const captureStorage = browser.storage.session || browser.storage.local;
const requestHeadersById = new Map();
const requestHeadersByUrl = new Map();
const responseFilenamesByUrl = new Map();
const mediaWriteQueues = new Map();
const modifierByTab = new Map();
let nativePort = null;
let nativeSequence = 0;
const nativePending = new Map();
const defaultSettings = {
  interceptDownloads: true,
  captureMedia: true,
  excludedHosts: "",
  excludedExtensions: "",
  includedExtensions: "",
  minDownloadBytes: 0,
  pauseInterception: false,
  bypassWhenModifier: true
};

function nativeRequest(message) {
  if (typeof browser.runtime.connectNative !== "function") return browser.runtime.sendNativeMessage(hostName, message);
  if (!nativePort) {
    nativePort = browser.runtime.connectNative(hostName);
    nativePort.onMessage.addListener((reply) => {
      const pending = nativePending.get(reply?.id);
      if (pending) { nativePending.delete(reply.id); pending.resolve(reply); }
    });
    nativePort.onDisconnect.addListener(() => {
      const error = new Error(browser.runtime.lastError?.message || "qtIDM native host disconnected.");
      for (const { reject } of nativePending.values()) reject(error);
      nativePending.clear(); nativePort = null;
    });
  }
  return new Promise((resolve, reject) => {
    const id = ++nativeSequence;
    nativePending.set(id, { resolve, reject });
    try { nativePort.postMessage({ ...message, id }); } catch (error) { nativePending.delete(id); reject(error); }
  });
}

async function settings() {
  return { ...defaultSettings, ...(await browser.storage.local.get(settingsKey))[settingsKey] };
}

function splitRules(value) {
  return String(value || "").split(/[\s,;]+/).map((item) => item.trim().toLowerCase()).filter(Boolean);
}

function isExcluded(url, value) {
  try {
    const parsed = new URL(url);
    const host = parsed.hostname.toLowerCase();
    const hostMatch = splitRules(value.excludedHosts).some((rule) => {
      const suffix = rule.startsWith("*.") ? rule.slice(2) : rule;
      return host === suffix || (rule.startsWith("*.") && host.endsWith(`.${suffix}`));
    });
    const extension = parsed.pathname.toLowerCase().match(/\.([a-z0-9]+)$/)?.[1] || "";
    return hostMatch || (extension && splitRules(value.excludedExtensions)
      .map((item) => item.replace(/^\./, "")).includes(extension));
  } catch (_) {
    return false;
  }
}

function extensionFor(url, filename = "") {
  const source = String(filename || new URL(url).pathname || "").toLowerCase();
  return source.match(/\.([a-z0-9]+)(?:$|[?#])/i)?.[1] || "";
}

function shouldIntercept(item, value) {
  const url = item.finalUrl || item.url;
  if (!value.interceptDownloads || value.pauseInterception || isExcluded(url, value)
      || (value.bypassWhenModifier && modifierByTab.get(item.tabId))) return false;
  const extension = extensionFor(url, item.filename || "");
  const included = splitRules(value.includedExtensions).map((item) => item.replace(/^\./, ""));
  if (included.length && (!extension || !included.includes(extension))) return false;
  const total = Number(item.totalBytes ?? item.fileSize ?? -1);
  return !(Number(value.minDownloadBytes) > 0 && total >= 0 && total < Number(value.minDownloadBytes));
}

function forwardedRequestHeaders(details) {
  const headers = {};
  const allowed = new Set([
    "accept", "accept-language", "authorization", "cookie", "origin", "referer", "user-agent"
  ]);
  for (const header of details.requestHeaders || []) {
    const name = String(header.name || "");
    const lower = name.toLowerCase();
    if ((allowed.has(lower) || lower.startsWith("sec-ch-ua") || lower.startsWith("sec-fetch-"))
        && typeof header.value === "string" && header.value) {
      headers[name] = header.value;
    }
  }
  return headers;
}

function rememberRequestHeaders(details) {
  const entry = requestHeadersByUrl.get(details.url) || { detectedAt: Date.now() };
  entry.headers = forwardedRequestHeaders(details);
  entry.detectedAt = Date.now();
  requestHeadersById.set(details.requestId, entry);
  while (requestHeadersById.size > 500) {
    requestHeadersById.delete(requestHeadersById.keys().next().value);
  }
  requestHeadersByUrl.delete(details.url);
  requestHeadersByUrl.set(details.url, entry);
  while (requestHeadersByUrl.size > 200) {
    requestHeadersByUrl.delete(requestHeadersByUrl.keys().next().value);
  }
  return entry.headers;
}

function textToBase64(value) {
  return btoa(unescape(encodeURIComponent(String(value || ""))));
}

function bytesToBase64(bytes) {
  const view = new Uint8Array(bytes);
  let binary = "";
  for (let offset = 0; offset < view.length; offset += 0x8000) binary += String.fromCharCode(...view.subarray(offset, offset + 0x8000));
  return btoa(binary);
}

function formRequestBody(details) {
  if (details.method !== "POST" || !details.requestBody?.formData) return "";
  const values = [];
  for (const [name, items] of Object.entries(details.requestBody.formData)) {
    for (const value of items || []) values.push(`${encodeURIComponent(name)}=${encodeURIComponent(value)}`);
  }
  return values.join("&");
}

function rawRequestBody(details) {
  if (details.method !== "POST") return "";
  const raw = details.requestBody?.raw;
  if (!Array.isArray(raw)) return "";
  const chunks = raw.filter((part) => part.bytes).map((part) => new Uint8Array(part.bytes));
  const size = chunks.reduce((total, chunk) => total + chunk.length, 0);
  if (!size) return "";
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
  return bytesToBase64(bytes);
}

function rememberRequestDetails(details) {
  const entry = requestHeadersByUrl.get(details.url) || {};
  entry.method = details.method === "POST" ? "POST" : "";
  entry.body = formRequestBody(details);
  entry.bodyBase64 = rawRequestBody(details);
  entry.detectedAt = Date.now();
  requestHeadersByUrl.set(details.url, entry);
}

function recentRequestHeaders(url) {
  const entry = requestHeadersByUrl.get(url);
  if (!entry || Date.now() - entry.detectedAt > 10 * 60 * 1000) {
    requestHeadersByUrl.delete(url);
    return {};
  }
  return entry.headers;
}

function recentRequestInfo(url) {
  const entry = requestHeadersByUrl.get(url);
  if (!entry || Date.now() - entry.detectedAt > 10 * 60 * 1000) return { headers: {}, method: "", body: "", bodyBase64: "" };
  return { headers: entry.headers || {}, method: entry.method || "", body: entry.body || "", bodyBase64: entry.bodyBase64 || "" };
}

function rememberResponseFilename(details) {
  const filename = responseFilename(details);
  if (!filename) return;
  responseFilenamesByUrl.delete(details.url);
  responseFilenamesByUrl.set(details.url, { filename, detectedAt: Date.now() });
  while (responseFilenamesByUrl.size > 200) {
    responseFilenamesByUrl.delete(responseFilenamesByUrl.keys().next().value);
  }
}

function recentResponseFilename(url) {
  const entry = responseFilenamesByUrl.get(url);
  if (!entry || Date.now() - entry.detectedAt > 10 * 60 * 1000) {
    responseFilenamesByUrl.delete(url);
    return "";
  }
  return entry.filename;
}

function hasHeader(headers, name) {
  return Object.keys(headers).some((key) => key.toLowerCase() === name.toLowerCase());
}

function setHeaderDefault(headers, name, value) {
  if (value && !hasHeader(headers, name)) headers[name] = value;
}

function leafFilename(value) {
  const normalized = String(value || "").replace(/\0/g, "").trim();
  if (!normalized) return "";
  return normalized.split("/").pop() || "";
}

function browserSuggestedFilename(filename, url) {
  const captured = leafFilename(filename);
  const response = recentResponseFilename(url);
  if (!captured || !response || captured === response) return captured || response;

  // Browsers reserve a local name before the extension can cancel the download,
  // so their temporary collision suffix must not become qtIDM's requested name.
  // qtIDM applies its own conflict policy against the real destination.
  const extensionAt = response.lastIndexOf(".");
  const stem = extensionAt > 0 ? response.slice(0, extensionAt) : response;
  const extension = extensionAt > 0 ? response.slice(extensionAt) : "";
  const collisionSuffix = captured.slice(stem.length, captured.length - extension.length);
  if (captured.startsWith(stem) && captured.endsWith(extension) && /^\(\d+\)$/.test(collisionSuffix)) {
    return response;
  }
  return captured;
}

function responseFilename(details) {
  const disposition = (details.responseHeaders || [])
    .find((header) => String(header.name || "").toLowerCase() === "content-disposition")?.value || "";
  const extended = disposition.match(/filename\*\s*=\s*(?:"([^"]+)"|([^;]+))/i);
  if (extended) {
    const encoded = (extended[1] || extended[2] || "").trim().split("'").slice(2).join("'")
      || (extended[1] || extended[2] || "").trim();
    try {
      return leafFilename(decodeURIComponent(encoded));
    } catch (_) {
      return leafFilename(encoded);
    }
  }
  const regular = disposition.match(/filename\s*=\s*(?:"((?:\\.|[^"])*)"|([^;]+))/i);
  return leafFilename((regular?.[1] || regular?.[2] || "").replace(/\\"/g, "\""));
}

function mediaSuggestedFilename(item, tab) {
  const captured = leafFilename(item?.suggestedFilename);
  if (captured && !/\.(?:m3u8|mpd)$/i.test(captured)) return captured;
  return leafFilename(String(tab?.title || "").replace(/\s+::\s+[^:]+$/, ""));
}

browser.contextMenus.create({
  id: "qtidm-link",
  title: "Download with qtIDM",
  contexts: ["link", "image", "video", "audio"]
});
browser.contextMenus.create({ id: "qtidm-all-links", title: "Download all links and images with qtIDM", contexts: ["page"] });
browser.contextMenus.create({ id: "qtidm-selected-links", title: "Download selected links with qtIDM", contexts: ["selection"] });

function showError(message) {
  console.error(`qtIDM: ${message}`);
  browser.browserAction.setBadgeText({ text: "!" }).catch(() => {});
  browser.browserAction.setTitle({ title: `qtIDM error: ${message}` }).catch(() => {});
}

function isManifestUrl(url) {
  return /\.m3u8(?:$|[?#])|\.mpd(?:$|[?#])/i.test(url || "");
}

function isSubtitleUrl(url) {
  return /\.(?:vtt|srt|ass|ssa|ttml)(?:$|[?#])/i.test(url || "");
}

function isManifestResponse(details) {
  return (details.responseHeaders || []).some((header) => {
    const value = String(header.value || "").toLowerCase();
    return header.name.toLowerCase() === "content-type"
      && (value.includes("mpegurl") || value.includes("dash+xml"));
  });
}

function isDirectMediaResponse(details) {
  if (details.type !== "media" || /\.(?:ts|m4s|aac)(?:$|[?#])/i.test(details.url || "")) return false;
  return (details.responseHeaders || []).some((header) => {
    const value = String(header.value || "").toLowerCase();
    return header.name.toLowerCase() === "content-type"
      && (value.startsWith("video/") || value.startsWith("audio/"));
  });
}

function isSubtitleResponse(details) {
  return (details.responseHeaders || []).some((header) => {
    const value = String(header.value || "").toLowerCase();
    return header.name.toLowerCase() === "content-type"
      && (value.includes("text/vtt") || value.includes("application/ttml+xml")
        || value.includes("application/x-subrip") || value.includes("text/x-ssa"));
  });
}

function mediaType(url, contentType = "") {
  if (/\.mpd(?:$|[?#])/i.test(url) || contentType.includes("dash+xml")) return "DASH";
  if (/\.m3u8(?:$|[?#])/i.test(url) || contentType.includes("mpegurl")) return "HLS";
  if (contentType.startsWith("video/")) return "Video";
  if (contentType.startsWith("audio/")) return "Audio";
  if (isSubtitleUrl(url) || contentType.includes("text/vtt") || contentType.includes("ttml")
      || contentType.includes("subrip") || contentType.includes("x-ssa")) return "Subtitle";
  return "Media";
}

async function storeMedia(details, value) {
  if (details.tabId < 0 || !details.url || isExcluded(details.url, value)) {
    return;
  }
  const key = mediaKey(details.tabId);
  const stored = (await captureStorage.get(key))[key];
  const items = Array.isArray(stored) ? stored : (stored && stored.url ? [stored] : []);
  const contentType = (details.responseHeaders || [])
    .find((header) => header.name.toLowerCase() === "content-type")?.value?.toLowerCase() || "";
  const duplicate = items.findIndex((entry) => entry.url === details.url);
  const previous = duplicate >= 0 ? items[duplicate] : null;
  const observedHeaders = details.requestHeaders
    ? forwardedRequestHeaders(details)
    : requestHeadersById.get(details.requestId)?.headers || {};
  const item = {
    url: details.url,
    referrer: details.documentUrl || details.originUrl
      || Object.entries(observedHeaders).find(([name]) => name.toLowerCase() === "referer")?.[1]
      || previous?.referrer || "",
    detectedAt: Date.now(),
    type: mediaType(details.url, contentType),
    headers: Object.keys(observedHeaders).length ? observedHeaders : previous?.headers || {},
    suggestedFilename: responseFilename(details) || previous?.suggestedFilename || ""
  };
  if (duplicate >= 0) items.splice(duplicate, 1);
  items.unshift(item);
  items.splice(25);
  await captureStorage.set({ [key]: items });
  await browser.browserAction.setBadgeText({ tabId: details.tabId, text: String(items.length) });
  await browser.browserAction.setTitle({
    tabId: details.tabId,
    title: "Open captured downloads in qtIDM"
  });
}

function rememberMedia(details, value) {
  const previous = mediaWriteQueues.get(details.tabId) || Promise.resolve();
  const operation = previous.catch(() => {}).then(() => storeMedia(details, value));
  mediaWriteQueues.set(details.tabId, operation);
  return operation.finally(() => {
    if (mediaWriteQueues.get(details.tabId) === operation) mediaWriteQueues.delete(details.tabId);
  });
}

async function makeDownloadEntry(url, tab, referrer = "", capturedHeaders = {}, mediaTypeHint = "", suggestedFilename = "", method = "", body = "") {
  if (!/^(?:https?|ftp):/i.test(url)) {
    throw new Error("Only HTTP, HTTPS, and FTP downloads can be redirected to qtIDM.");
  }
  let cookies = [];
  try {
    cookies = await browser.cookies.getAll({ url });
  } catch (_) {
    cookies = [];
  }
  const cookieHeader = cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  const request = recentRequestInfo(url);
  const headers = { ...request.headers, ...capturedHeaders };
  setHeaderDefault(headers, "Cookie", cookieHeader);
  setHeaderDefault(headers, "Referer", referrer || (tab && tab.url ? tab.url : ""));
  setHeaderDefault(headers, "User-Agent", navigator.userAgent);
  if (mediaTypeHint === "HLS" || mediaTypeHint === "DASH") headers._qtidmMediaType = mediaTypeHint;
  return { url, headers, method: method === "POST" || request.method === "POST" ? "POST" : "", body: body ? textToBase64(body) : (request.bodyBase64 || textToBase64(request.body)), suggestedFilename: leafFilename(suggestedFilename) };
}

async function sendToHost(url, tab, referrer = "", capturedHeaders = {}, mediaTypeHint = "", suggestedFilename = "", method = "", body = "") {
  const response = await nativeRequest({ downloads: [await makeDownloadEntry(url, tab, referrer, capturedHeaders, mediaTypeHint, suggestedFilename, method, body)] });
  if (!response || !response.ok || response.accepted !== true) {
    throw new Error(response && response.message ? response.message : "The qtIDM native host did not acknowledge the request.");
  }
}

async function prepareBrowserDownload(url) {
  const response = await nativeRequest({ url, prepare: true });
  if (!response || !response.ok) {
    throw new Error(response?.message || "The qtIDM native host is not ready to receive the download.");
  }
  return response.prepared === true;
}

async function sendBatchToHost(urls, tab) {
  const response = await nativeRequest({ downloads: await Promise.all(urls.map((url) => makeDownloadEntry(url, tab, tab?.url || ""))) });
  if (!response || !response.ok || response.accepted !== true) {
    throw new Error(response?.message || "The qtIDM native host did not acknowledge the batch.");
  }
}

browser.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === "qtidm-all-links" || info.menuItemId === "qtidm-selected-links") {
    const selected = info.menuItemId === "qtidm-selected-links";
    const code = selected
      ? `(() => { const wrapper=document.createElement('div'); const selection=window.getSelection(); for(let i=0;selection&&i<selection.rangeCount;++i) wrapper.append(selection.getRangeAt(i).cloneContents()); return [...new Set([...wrapper.querySelectorAll('a[href]')].map(e=>e.href).filter(url=>/^(?:https?|ftp):/i.test(url)))].slice(0,100); })()`
      : `(() => [...new Set([...document.querySelectorAll('a[href],img[src]')].map(e=>e.href||e.src).filter(url=>/^(?:https?|ftp):/i.test(url)))].slice(0,100))()`;
    browser.tabs.executeScript(tab.id, { frameId: info.frameId, code })
      .then((results) => sendBatchToHost(results.flat(), tab)).catch((error) => showError(error.message));
    return;
  }
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (url) {
    sendToHost(url, tab).catch((error) => showError(error.message));
  }
});

async function redirectBrowserDownload(item, url) {
  let paused = false;
  let removed = false;
  const request = recentRequestInfo(url);
  try {
    await browser.downloads.pause(item.id);
    paused = true;
  } catch (_) {
    // Very small downloads can finish before the pause request reaches Firefox.
  }

  try {
    const prepared = await prepareBrowserDownload(url);
    if (prepared) {
      await browser.downloads.cancel(item.id);
      await browser.downloads.removeFile(item.id).catch(() => {});
      await browser.downloads.erase({ id: item.id });
      removed = true;
    }
    await sendToHost(
      url, null, item.referrer || "", {}, "", browserSuggestedFilename(item.filename, url), request.method, request.body
    );
    if (!prepared) {
      await browser.downloads.cancel(item.id).catch(() => {});
      await browser.downloads.removeFile(item.id).catch(() => {});
      await browser.downloads.erase({ id: item.id });
    }
  } catch (error) {
    if (removed) {
      const restore = { url: item.url || url };
      if (request.method === "POST") { restore.method = "POST"; restore.body = request.body; }
      await browser.downloads.download(restore).catch(() => {});
    } else if (paused) {
      await browser.downloads.resume(item.id).catch(() => {});
    }
    throw error;
  }

}

browser.downloads.onCreated.addListener((item) => {
  const url = item.finalUrl || item.url;
  if (!url || item.byExtensionId === browser.runtime.id || !/^https?:/i.test(url)) {
    return;
  }
  settings()
    .then((value) => shouldIntercept(item, value)
      ? redirectBrowserDownload(item, url)
      : undefined)
    .catch((error) => showError(`Could not redirect the browser download: ${error.message}`));
});

browser.webRequest.onBeforeRequest.addListener(
  (details) => {
    rememberRequestDetails(details);
    settings().then((value) => {
      if (value.captureMedia && (isManifestUrl(details.url) || isSubtitleUrl(details.url))) return rememberMedia(details, value);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame", "xmlhttprequest", "media", "other"] },
  ["requestBody"]
);

browser.webRequest.onBeforeSendHeaders.addListener(
  (details) => {
    rememberRequestHeaders(details);
    settings().then((value) => {
      if (value.captureMedia && (isManifestUrl(details.url) || isSubtitleUrl(details.url))) {
        return rememberMedia(details, value);
      }
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] },
  ["requestHeaders"]
);

browser.webRequest.onHeadersReceived.addListener(
  (details) => {
    rememberResponseFilename(details);
    settings().then((value) => {
      if (value.captureMedia && (isManifestResponse(details) || isDirectMediaResponse(details)
          || isSubtitleResponse(details))) return rememberMedia(details, value);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] },
  ["responseHeaders"]
);

browser.runtime.onMessage.addListener(async (message, sender) => {
  if (message.type === "modifierState") {
    if (sender.tab?.id >= 0) modifierByTab.set(sender.tab.id, !!message.active);
    return { ok: true };
  }
  if (message.type === "qtidmScheme") {
    const tab = sender.tab || await browser.tabs.get(message.tabId);
    await sendToHost(message.url, tab, tab.url);
    return { ok: true };
  }
  const key = mediaKey(message.tabId);
  if (message.type === "listMedia") {
    const stored = (await captureStorage.get(key))[key];
    return { ok: true, items: Array.isArray(stored) ? stored : (stored?.url ? [stored] : []) };
  }
  if (message.type === "getSettings") {
    return { ok: true, settings: await settings() };
  }
  if (message.type === "saveSettings") {
    const value = { ...defaultSettings, ...message.settings };
    await browser.storage.local.set({ [settingsKey]: value });
    return { ok: true, settings: value };
  }
  if (message.type === "addManual") {
    try {
      const tab = await browser.tabs.get(message.tabId);
      await sendToHost(message.url, tab, tab.url);
      return { ok: true };
    } catch (error) {
      showError(error.message);
      return { ok: false, message: error.message };
    }
  }
  if (message.type === "addManualBatch") {
    try {
      const tab = await browser.tabs.get(message.tabId);
      await sendBatchToHost(message.urls, tab);
      return { ok: true };
    } catch (error) {
      showError(error.message);
      return { ok: false, message: error.message };
    }
  }
  if (message.type === "clearMedia") {
    await captureStorage.remove(key);
    await browser.browserAction.setBadgeText({ tabId: message.tabId, text: "" });
    return { ok: true };
  }
  if (message.type === "downloadMedia") {
    const stored = (await captureStorage.get(key))[key];
    const items = Array.isArray(stored) ? stored : [];
    const item = items[message.index];
    if (!item) return { ok: false, message: "The selected media capture no longer exists." };
    const tab = await browser.tabs.get(message.tabId);
    try {
      await sendToHost(item.url, tab, item.referrer, item.headers, item.type, mediaSuggestedFilename(item, tab));
      return { ok: true };
    } catch (error) {
      showError(error.message);
      return { ok: false, message: error.message };
    }
  }
  return undefined;
});

browser.tabs.onRemoved.addListener((tabId) => {
  modifierByTab.delete(tabId);
  captureStorage.remove(mediaKey(tabId)).catch(() => {});
});
