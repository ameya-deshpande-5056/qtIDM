const hostName = "io.github.qtidm.native";
const mediaKey = (tabId) => `qtidm-media-${tabId}`;
const settingsKey = "qtidm-settings";
const defaultSettings = { interceptDownloads: true, captureMedia: true };

async function settings() {
  return { ...defaultSettings, ...(await chrome.storage.local.get(settingsKey))[settingsKey] };
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "qtidm-link",
    title: "Download with qtIDM",
    contexts: ["link", "image", "video", "audio"]
  });
});

function showError(message) {
  console.error(`qtIDM: ${message}`);
  chrome.action.setBadgeText({ text: "!" }).catch(() => {});
  chrome.action.setTitle({ title: `qtIDM error: ${message}` }).catch(() => {});
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

async function rememberMedia(details) {
  if (details.tabId < 0 || !details.url) {
    return;
  }
  const key = mediaKey(details.tabId);
  const stored = (await chrome.storage.local.get(key))[key];
  const items = Array.isArray(stored) ? stored : (stored && stored.url ? [stored] : []);
  const contentType = (details.responseHeaders || [])
    .find((header) => header.name.toLowerCase() === "content-type")?.value?.toLowerCase() || "";
  const item = {
    url: details.url,
    referrer: details.documentUrl || details.initiator || "",
    detectedAt: Date.now(),
    type: mediaType(details.url, contentType)
  };
  const duplicate = items.findIndex((entry) => entry.url === item.url);
  if (duplicate >= 0) items.splice(duplicate, 1);
  items.unshift(item);
  items.splice(25);
  await chrome.storage.local.set({ [key]: items });
  await chrome.action.setBadgeText({ tabId: details.tabId, text: String(items.length) });
  await chrome.action.setTitle({
    tabId: details.tabId,
    title: "Open captured downloads in qtIDM"
  });
}

async function sendToHost(url, tab, referrer = "") {
  if (!/^https?:/i.test(url)) {
    throw new Error("Only HTTP and HTTPS downloads can be redirected to qtIDM.");
  }
  const cookies = await chrome.cookies.getAll({ url }).catch(() => []);
  const cookieHeader = cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  const response = await chrome.runtime.sendNativeMessage(hostName, {
    url,
    headers: {
      Cookie: cookieHeader,
      Referer: referrer || tab?.url || "",
      "User-Agent": navigator.userAgent
    }
  });
  if (!response || !response.ok) {
    throw new Error(response && response.message ? response.message : "The qtIDM native host did not acknowledge the request.");
  }
}

async function sendBatchToHost(urls, tab) {
  const response = await chrome.runtime.sendNativeMessage(hostName, {
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

chrome.contextMenus.onClicked.addListener((info, tab) => {
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (url) {
    sendToHost(url, tab).catch((error) => showError(error.message));
  }
});

chrome.downloads.onCreated.addListener((item) => {
  const url = item.finalUrl || item.url;
  if (!url || item.byExtensionId === chrome.runtime.id || !/^https?:/i.test(url)) {
    return;
  }
  settings()
    .then((value) => value.interceptDownloads ? chrome.downloads.cancel(item.id) : false)
    .then((canceled) => canceled === false ? undefined : sendToHost(url, null, item.referrer || ""))
    .catch((error) => showError(`Could not redirect the browser download: ${error.message}`));
});

chrome.webRequest.onBeforeRequest.addListener(
  (details) => {
    settings().then((value) => {
      if (value.captureMedia && (isManifestUrl(details.url) || isSubtitleUrl(details.url))) return rememberMedia(details);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] }
);

chrome.webRequest.onHeadersReceived.addListener(
  (details) => {
    settings().then((value) => {
      if (value.captureMedia && (isManifestResponse(details) || isDirectMediaResponse(details)
          || isSubtitleResponse(details))) return rememberMedia(details);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["xmlhttprequest", "media", "other"] },
  ["responseHeaders"]
);

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  (async () => {
    const key = mediaKey(message.tabId);
    if (message.type === "listMedia") {
      const stored = (await chrome.storage.local.get(key))[key];
      sendResponse({ ok: true, items: Array.isArray(stored) ? stored : (stored?.url ? [stored] : []) });
      return;
    }
    if (message.type === "getSettings") {
      sendResponse({ ok: true, settings: await settings() });
      return;
    }
    if (message.type === "saveSettings") {
      const value = { ...defaultSettings, ...message.settings };
      await chrome.storage.local.set({ [settingsKey]: value });
      sendResponse({ ok: true, settings: value });
      return;
    }
    if (message.type === "addManual") {
      try {
        const tab = await chrome.tabs.get(message.tabId);
        await sendToHost(message.url, tab, tab.url);
        sendResponse({ ok: true });
      } catch (error) {
        showError(error.message);
        sendResponse({ ok: false, message: error.message });
      }
      return;
    }
    if (message.type === "addManualBatch") {
      try {
        const tab = await chrome.tabs.get(message.tabId);
        await sendBatchToHost(message.urls, tab);
        sendResponse({ ok: true });
      } catch (error) {
        showError(error.message);
        sendResponse({ ok: false, message: error.message });
      }
      return;
    }
    if (message.type === "clearMedia") {
      await chrome.storage.local.remove(key);
      await chrome.action.setBadgeText({ tabId: message.tabId, text: "" });
      sendResponse({ ok: true });
      return;
    }
    if (message.type === "downloadMedia") {
      const stored = (await chrome.storage.local.get(key))[key];
      const items = Array.isArray(stored) ? stored : [];
      const item = items[message.index];
      if (!item) {
        sendResponse({ ok: false, message: "The selected media capture no longer exists." });
        return;
      }
      try {
        const tab = await chrome.tabs.get(message.tabId);
        await sendToHost(item.url, tab, item.referrer);
        sendResponse({ ok: true });
      } catch (error) {
        showError(error.message);
        sendResponse({ ok: false, message: error.message });
      }
    }
  })().catch((error) => sendResponse({ ok: false, message: error.message }));
  return true;
});

chrome.tabs.onRemoved.addListener((tabId) => {
  chrome.storage.local.remove(mediaKey(tabId)).catch(() => {});
});
