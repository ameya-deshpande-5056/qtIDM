const hostName = "io.github.qtidm.native";
const mediaKey = (tabId) => `qtidm-media-${tabId}`;
const settingsKey = "qtidm-settings";
const captureStorage = browser.storage.session || browser.storage.local;
const requestHeadersById = new Map();
const requestHeadersByUrl = new Map();
const mediaWriteQueues = new Map();
const defaultSettings = {
  interceptDownloads: true,
  captureMedia: true,
  excludedHosts: "",
  excludedExtensions: ""
};

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
  const entry = {
    headers: forwardedRequestHeaders(details),
    detectedAt: Date.now()
  };
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

function recentRequestHeaders(url) {
  const entry = requestHeadersByUrl.get(url);
  if (!entry || Date.now() - entry.detectedAt > 10 * 60 * 1000) {
    requestHeadersByUrl.delete(url);
    return {};
  }
  return entry.headers;
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

async function sendToHost(url, tab, referrer = "", capturedHeaders = {}, mediaTypeHint = "", suggestedFilename = "") {
  if (!/^https?:/i.test(url)) {
    throw new Error("Only HTTP and HTTPS downloads can be redirected to qtIDM.");
  }
  let cookies = [];
  try {
    cookies = await browser.cookies.getAll({ url });
  } catch (_) {
    cookies = [];
  }
  const cookieHeader = cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  const headers = { ...recentRequestHeaders(url), ...capturedHeaders };
  setHeaderDefault(headers, "Cookie", cookieHeader);
  setHeaderDefault(headers, "Referer", referrer || (tab && tab.url ? tab.url : ""));
  setHeaderDefault(headers, "User-Agent", navigator.userAgent);
  const response = await browser.runtime.sendNativeMessage(hostName, {
    url,
    mediaType: mediaTypeHint === "HLS" || mediaTypeHint === "DASH" ? mediaTypeHint : "",
    suggestedFilename: leafFilename(suggestedFilename),
    headers
  });
  if (!response || !response.ok) {
    throw new Error(response && response.message ? response.message : "The qtIDM native host did not acknowledge the request.");
  }
}

async function sendBatchToHost(urls, tab) {
  const response = await browser.runtime.sendNativeMessage(hostName, {
    urls,
    headers: {
      Referer: tab?.url || "",
      "User-Agent": navigator.userAgent
    }
  });
  if (!response || !response.ok) {
    throw new Error(response?.message || "The qtIDM native host did not acknowledge the batch.");
  }
}

browser.contextMenus.onClicked.addListener((info, tab) => {
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (url) {
    sendToHost(url, tab).catch((error) => showError(error.message));
  }
});

async function redirectBrowserDownload(item, url) {
  let paused = false;
  try {
    await browser.downloads.pause(item.id);
    paused = true;
  } catch (_) {
    // Very small downloads can finish before the pause request reaches Firefox.
  }

  try {
    await sendToHost(url, null, item.referrer || "", {}, "", item.filename || "");
  } catch (error) {
    if (paused) {
      await browser.downloads.resume(item.id).catch(() => {});
    }
    throw error;
  }

  await browser.downloads.cancel(item.id).catch(() => {});
  await browser.downloads.removeFile(item.id).catch(() => {});
  await browser.downloads.erase({ id: item.id });
}

browser.downloads.onCreated.addListener((item) => {
  const url = item.finalUrl || item.url;
  if (!url || item.byExtensionId === browser.runtime.id || !/^https?:/i.test(url)) {
    return;
  }
  settings()
    .then((value) => value.interceptDownloads && !isExcluded(url, value)
      ? redirectBrowserDownload(item, url)
      : undefined)
    .catch((error) => showError(`Could not redirect the browser download: ${error.message}`));
});

browser.webRequest.onBeforeRequest.addListener(
  (details) => {
    settings().then((value) => {
      if (value.captureMedia && (isManifestUrl(details.url) || isSubtitleUrl(details.url))) return rememberMedia(details, value);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] }
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
    settings().then((value) => {
      if (value.captureMedia && (isManifestResponse(details) || isDirectMediaResponse(details)
          || isSubtitleResponse(details))) return rememberMedia(details, value);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] },
  ["responseHeaders"]
);

browser.runtime.onMessage.addListener(async (message) => {
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
  captureStorage.remove(mediaKey(tabId)).catch(() => {});
});
