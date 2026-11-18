const hostName = "io.qtidm.native";
const mediaKey = (tabId) => `qtidm-media-${tabId}`;
const settingsKey = "qtidm-settings";
const captureStorage = chrome.storage.session || chrome.storage.local;
const requestHeadersById = new Map();
const requestHeadersByUrl = new Map();
const responseFilenamesByUrl = new Map();
const mediaWriteQueues = new Map();
const modifierByTab = new Map();
const restoredDownloads = new Map();
let nativePort = null;
let nativeSequence = 0;
let swStreamSequence = 0;
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
  // Keeping a port open lets the host retain its D-Bus session and avoids a
  // process launch for every download.  The fallback keeps the module usable
  // in harnesses and browsers that only expose one-shot native messaging.
  if (typeof chrome.runtime.connectNative !== "function") {
    return chrome.runtime.sendNativeMessage(hostName, message);
  }
  if (!nativePort) {
    nativePort = chrome.runtime.connectNative(hostName);
    nativePort.onMessage.addListener((reply) => {
      const pending = nativePending.get(reply?.id);
      if (!pending) return;
      nativePending.delete(reply.id);
      pending.resolve(reply);
    });
    nativePort.onDisconnect.addListener(() => {
      const error = new Error(chrome.runtime.lastError?.message || "qtIDM native host disconnected.");
      for (const { reject } of nativePending.values()) reject(error);
      nativePending.clear();
      nativePort = null;
    });
  }
  return new Promise((resolve, reject) => {
    const id = ++nativeSequence;
    nativePending.set(id, { resolve, reject });
    try { nativePort.postMessage({ ...message, id }); }
    catch (error) { nativePending.delete(id); reject(error); }
  });
}

async function settings() {
  return { ...defaultSettings, ...(await chrome.storage.local.get(settingsKey))[settingsKey] };
}

function splitRules(value) {
  return String(value || "").split(/[\s,;]+/).map((item) => item.trim().toLowerCase()).filter(Boolean);
}

function isExcluded(url, value) {
  try {
    const parsed = new URL(url);
    let host = parsed.hostname.toLowerCase();
    let pathnameForExt = parsed.pathname;
    if (parsed.protocol === "blob:") {
      try {
        const inner = new URL(parsed.pathname);
        host = inner.hostname.toLowerCase();
        pathnameForExt = inner.pathname;
      } catch (_) {}
    }
    const hostMatch = splitRules(value.excludedHosts).some((rule) => {
      const suffix = rule.startsWith("*.") ? rule.slice(2) : rule;
      return host === suffix || (rule.startsWith("*.") && host.endsWith(`.${suffix}`));
    });
    const extension = pathnameForExt.toLowerCase().match(/\.([a-z0-9]+)$/)?.[1] || "";
    return Boolean(hostMatch || (extension && splitRules(value.excludedExtensions)
      .map((item) => item.replace(/^\./, "")).includes(extension)));
  } catch (_) {
    return false;
  }
}

function extensionFor(url, filename = "") {
  try {
    const source = String(filename || (url ? new URL(url).pathname : "") || "").toLowerCase();
    return source.match(/\.([a-z0-9]+)(?:$|[?#])/i)?.[1] || "";
  } catch (_) {
    const fallback = String(filename || url || "").toLowerCase();
    return fallback.match(/\.([a-z0-9]+)(?:$|[?#])/i)?.[1] || "";
  }
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

function markRestoredDownload(url) {
  restoredDownloads.set(url, Date.now() + 10000);
}

function consumeRestoredDownload(url) {
  const expiresAt = restoredDownloads.get(url);
  if (!expiresAt) return false;
  restoredDownloads.delete(url);
  return expiresAt >= Date.now();
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
  // Avoid exceeding apply()/argument limits on large request bodies.
  for (let offset = 0; offset < view.length; offset += 0x8000) {
    binary += String.fromCharCode(...view.subarray(offset, offset + 0x8000));
  }
  return btoa(binary);
}

function formRequestBody(details) {
  if (details.method !== "POST") return "";
  const fields = details.requestBody?.formData;
  if (!fields) return "";
  const values = [];
  for (const [name, items] of Object.entries(fields)) {
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
  requestHeadersByUrl.delete(details.url);
  requestHeadersByUrl.set(details.url, entry);
  while (requestHeadersByUrl.size > 200) requestHeadersByUrl.delete(requestHeadersByUrl.keys().next().value);
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

function httpOrigin(value) {
  try {
    const parsed = new URL(value);
    return /^https?:$/.test(parsed.protocol) ? parsed.origin : "";
  } catch (_) {
    return "";
  }
}

function firstHttpUrl(...candidates) {
  for (const candidate of candidates) {
    if (typeof candidate !== "string" || !candidate) continue;
    try {
      const parsed = new URL(candidate);
      if (parsed.protocol === "http:" || parsed.protocol === "https:") {
        return parsed.href;
      }
    } catch (_) {}
  }
  return "";
}

function leafFilename(value) {
  const normalized = String(value || "").replace(/\0/g, "").trim();
  if (!normalized) return "";
  return normalized.split("/").pop() || "";
}

function extensionFromMime(mime) {
  if (!mime) return "";
  const cleaned = String(mime).split(";")[0].trim().toLowerCase();
  switch (cleaned) {
    // Audio
    case "audio/mpeg":
    case "audio/mp3":
      return "mp3";
    case "audio/mp4":
    case "audio/x-m4a":
    case "audio/m4a":
      return "m4a";
    case "audio/aac":
    case "audio/x-aac":
    case "audio/aacp":
      return "aac";
    case "audio/ogg":
    case "audio/vorbis":
    case "application/ogg":
      return "ogg";
    case "audio/opus":
    case "audio/x-opus":
      return "opus";
    case "audio/flac":
    case "audio/x-flac":
      return "flac";
    case "audio/wav":
    case "audio/x-wav":
    case "audio/wave":
      return "wav";
    case "audio/webm":
      return "webm";

    // Video
    case "video/mp4":
      return "mp4";
    case "video/webm":
      return "webm";
    case "video/x-matroska":
      return "mkv";
    case "video/quicktime":
      return "mov";
    case "video/x-flv":
      return "flv";
    case "video/x-msvideo":
      return "avi";
    case "video/3gpp":
      return "3gp";
    case "video/mp2t":
      return "ts";

    // Adaptive manifests
    case "application/vnd.apple.mpegurl":
    case "application/x-mpegurl":
    case "audio/mpegurl":
      return "m3u8";
    case "application/dash+xml":
      return "mpd";

    // Subtitles
    case "text/vtt":
      return "vtt";
    case "application/x-subrip":
      return "srt";
    case "application/ttml+xml":
      return "ttml";
    case "text/x-ssa":
    case "text/x-ass":
      return "ass";

    // Documents & archives
    case "application/pdf":
      return "pdf";
    case "application/zip":
    case "application/x-zip-compressed":
      return "zip";
    case "application/x-tar":
      return "tar";
    case "application/gzip":
    case "application/x-gzip":
      return "gz";
    case "application/x-7z-compressed":
      return "7z";
    case "application/vnd.rar":
    case "application/x-rar-compressed":
      return "rar";
    case "application/epub+zip":
      return "epub";
    case "text/csv":
      return "csv";
    case "text/plain":
      return "txt";
    case "application/json":
      return "json";

    // Images
    case "image/png":
      return "png";
    case "image/jpeg":
      return "jpg";
    case "image/webp":
      return "webp";
    case "image/gif":
      return "gif";

    default:
      if (cleaned.startsWith("audio/")) return "mp3";
      if (cleaned.startsWith("video/")) return "mp4";
      return "";
  }
}

function browserSuggestedFilename(filename, url, mimeType = "") {
  const captured = leafFilename(filename);
  const response = recentResponseFilename(url);
  let chosen = (!captured || !response || captured === response) ? (captured || response) : captured;

  // Browsers reserve a local name before the extension can cancel the download,
  // so their temporary collision suffix must not become qtIDM's requested name.
  // qtIDM applies its own conflict policy against the real destination.
  if (captured && response && captured !== response) {
    const extensionAt = response.lastIndexOf(".");
    const stem = extensionAt > 0 ? response.slice(0, extensionAt) : response;
    const extension = extensionAt > 0 ? response.slice(extensionAt) : "";
    const collisionSuffix = captured.slice(stem.length, captured.length - extension.length);
    if (captured.startsWith(stem) && captured.endsWith(extension) && /^\(\d+\)$/.test(collisionSuffix)) {
      chosen = response;
    }
  }

  if (chosen) {
    const existingExt = extensionFor("", chosen);
    if (!existingExt) {
      const urlExt = extensionFor(url);
      if (urlExt && !/^(?:php|aspx?|cgi|jsp|html?)$/i.test(urlExt)) {
        chosen = `${chosen}.${urlExt}`;
      } else {
        const mimeExt = extensionFromMime(mimeType);
        if (mimeExt) {
          chosen = `${chosen}.${mimeExt}`;
        }
      }
    }
  }
  return chosen;
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

function sanitizeMediaTitle(rawTitle) {
  let title = String(rawTitle || "")
    .replace(/\0/g, "")
    .replace(/\s+::\s+[^:]+$/, "")
    .trim();
  if (!title) return "";
  title = title
    .replace(/[\x00-\x1f\x7f]/g, "")
    .replace(/[/\\]+/g, " - ")
    .replace(/:+/g, " - ")
    .replace(/"/g, "'")
    .replace(/\?/g, "")
    .replace(/\*/g, "_")
    .replace(/</g, "_")
    .replace(/>/g, "_")
    .replace(/\|/g, "-")
    .replace(/\s+/g, " ")
    .replace(/\s*-\s*-+/g, " - ")
    .trim();
  while (title.endsWith(".") || title.endsWith(" ") || title.endsWith("-")) {
    title = title.slice(0, -1).trim();
  }
  return title;
}

function mediaExtension(item) {
  const urlExt = extensionFor(item?.url);
  if (urlExt && !/^(?:php|aspx?|cgi|jsp|html?|m3u8|mpd)$/i.test(urlExt)) {
    return urlExt;
  }
  const capturedExt = extensionFor("", item?.suggestedFilename);
  if (capturedExt && !/^(?:m3u8|mpd)$/i.test(capturedExt)) {
    return capturedExt;
  }
  const mimeExt = extensionFromMime(item?.contentType);
  if (mimeExt && !/^(?:m3u8|mpd)$/i.test(mimeExt)) {
    return mimeExt;
  }
  const type = (item?.type || "").toUpperCase();
  if (type === "VIDEO") return "mp4";
  if (type === "AUDIO") return "mp3";
  if (type === "SUBTITLE") return "vtt";
  if (type === "DOCUMENT") return "pdf";
  if (type === "ARCHIVE") return "zip";
  if (type === "IMAGE") return "png";
  return "";
}

function mediaSuggestedFilename(item, tab) {
  const ext = mediaExtension(item);
  const withExt = (title) => {
    if (!title) return "";
    const existingExt = extensionFor("", title);
    if (existingExt && !/^(?:m3u8|mpd)$/i.test(existingExt)) {
      if (existingExt === ext) return title;
      if (/^(?:php|aspx?|cgi|jsp|html?|bin)$/i.test(existingExt)) {
        return ext ? `${title.slice(0, -(existingExt.length + 1))}.${ext}` : title;
      }
      if (ext && /^(?:mp4|mp3|m4a|webm|opus|ogg|flac|wav|mkv|mov|avi)$/i.test(existingExt)) {
        return `${title.slice(0, -(existingExt.length + 1))}.${ext}`;
      }
      return title;
    }
    return ext ? `${title}.${ext}` : title;
  };

  const captured = leafFilename(item?.suggestedFilename);
  if (captured && !/\.(?:m3u8|mpd)$/i.test(captured)) {
    return withExt(sanitizeMediaTitle(captured));
  }
  const fromTitle = sanitizeMediaTitle(tab?.title);
  if (fromTitle) {
    return withExt(fromTitle);
  }
  if (captured) {
    const stem = captured.replace(/\.(?:m3u8|mpd)$/i, "").trim();
    if (stem && !/^(?:index|master|playlist|manifest|uwu|stream|video|audio)$/i.test(stem)) {
      return withExt(sanitizeMediaTitle(stem));
    }
  }
  try {
    const pathLeaf = new URL(item?.url || "").pathname.split("/").pop() || "";
    const cleanLeaf = decodeURIComponent(pathLeaf).trim();
    const leafExt = extensionFor("", cleanLeaf);
    const stem = leafExt ? cleanLeaf.slice(0, -(leafExt.length + 1)) : cleanLeaf;
    if (stem && !/^(?:index|master|playlist|manifest|stream|video|audio|videoplayback|playback)$/i.test(stem)) {
      return withExt(sanitizeMediaTitle(stem));
    }
  } catch (_) {}

  return ext ? `media.${ext}` : "";
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
  chrome.contextMenus.create({
    id: "qtidm-link",
    title: "Download with qtIDM",
    contexts: ["link", "image", "video", "audio"]
  });
  chrome.contextMenus.create({ id: "qtidm-all-links", title: "Download all links and images with qtIDM", contexts: ["page"] });
  chrome.contextMenus.create({ id: "qtidm-selected-links", title: "Download selected links with qtIDM", contexts: ["selection"] });
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
  if (/\.(?:ts|m4s|aac|key|vtt)(?:$|[?#])/i.test(details.url || "")) return false;
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

async function resolveTabId(details) {
  if (typeof details?.tabId === "number" && details.tabId >= 0) return details.tabId;
  const initiator = details?.initiator || details?.documentUrl || "";
  if (initiator && chrome.tabs?.query) {
    try {
      const initOrigin = new URL(initiator).origin.toLowerCase();
      const tabs = await chrome.tabs.query({}).catch(() => []);
      const activeMatch = tabs.find((t) => t.active && t.url && new URL(t.url).origin.toLowerCase() === initOrigin);
      if (activeMatch?.id >= 0) return activeMatch.id;
      const anyMatch = tabs.find((t) => t.url && new URL(t.url).origin.toLowerCase() === initOrigin);
      if (anyMatch?.id >= 0) return anyMatch.id;
    } catch (_) {}
  }
  if (chrome.tabs?.query) {
    try {
      const activeTabs = await chrome.tabs.query({ active: true, lastFocusedWindow: true }).catch(() => []);
      if (activeTabs?.[0]?.id >= 0) return activeTabs[0].id;
    } catch (_) {}
  }
  return -1;
}

async function storeMedia(details, value) {
  const tabId = await resolveTabId(details);
  const isBlob = typeof details.url === "string" && details.url.startsWith("blob:");
  if (tabId < 0 || !details.url || (!/^https?:\/\//i.test(details.url) && !isBlob) || isExcluded(details.url, value)) {
    return;
  }
  const key = mediaKey(tabId);
  const stored = (await captureStorage.get(key))[key];
  const items = Array.isArray(stored) ? stored : (stored && stored.url ? [stored] : []);
  const contentType = (details.responseHeaders || [])
    .find((header) => header.name.toLowerCase() === "content-type")?.value?.toLowerCase() || details.mimeType || "";
  const duplicate = items.findIndex((entry) => entry.url === details.url);
  const previous = duplicate >= 0 ? items[duplicate] : null;
  const observedHeaders = details.requestHeaders
    ? forwardedRequestHeaders(details)
    : (details.requestId ? requestHeadersById.get(details.requestId)?.headers : null)
      || recentRequestHeaders(details.url)
      || {};
  const observedReferer = Object.entries(observedHeaders).find(([name]) => name.toLowerCase() === "referer")?.[1];
  const frameId = typeof details.frameId === "number" && details.frameId >= 0
    ? details.frameId
    : (typeof previous?.frameId === "number" ? previous.frameId : 0);
  const isSubframe = Boolean(details.isSubframe || frameId > 0 || previous?.isSubframe);
  const frameUrl = firstHttpUrl(details.frameUrl, isSubframe ? details.documentUrl : "", previous?.frameUrl);
  const item = {
    url: details.url,
    isBlob: isBlob || !!details.isBlob,
    size: typeof details.size === "number" ? details.size : (previous?.size || 0),
    referrer: firstHttpUrl(observedReferer, details.documentUrl, details.initiator, frameUrl, previous?.referrer)
      || observedReferer || details.initiator || details.documentUrl || previous?.referrer || "",
    frameId,
    frameUrl,
    frameToken: details.frameToken || previous?.frameToken || "",
    frameTitle: details.frameTitle || previous?.frameTitle || "",
    isSubframe,
    detectedAt: Date.now(),
    type: details.mediaType || mediaType(details.url, contentType),
    contentType: contentType || previous?.contentType || "",
    headers: Object.keys(observedHeaders).length ? observedHeaders : previous?.headers || {},
    suggestedFilename: details.filename || responseFilename(details) || previous?.suggestedFilename || ""
  };
  if (duplicate >= 0) items.splice(duplicate, 1);
  items.unshift(item);
  items.splice(25);
  await captureStorage.set({ [key]: items });
  await chrome.action.setBadgeText({ tabId, text: String(items.length) });
  await chrome.action.setTitle({
    tabId,
    title: "Open captured downloads in qtIDM"
  });
  chrome.runtime.sendMessage?.({ type: "mediaCaptured", tabId })?.catch?.(() => {});
}

async function pruneUnloadedFrameBlobs(tabId, frameToken = "", frameId = -1) {
  if (typeof tabId !== "number" || tabId < 0) return;
  if (!frameToken && !(typeof frameId === "number" && frameId > 0)) return;
  const key = mediaKey(tabId);
  const stored = (await captureStorage.get(key))[key];
  const items = Array.isArray(stored) ? stored : (stored && stored.url ? [stored] : []);
  if (!items.length) return;
  const kept = items.filter((entry) => {
    const isBlobEntry = Boolean(entry?.isBlob || (typeof entry?.url === "string" && entry.url.startsWith("blob:")));
    if (!isBlobEntry) return true;
    if (frameToken && entry.frameToken === frameToken) return false;
    if (frameId > 0 && entry.frameId === frameId) return false;
    return true;
  });
  if (kept.length === items.length) return;
  await captureStorage.set({ [key]: kept });
  await chrome.action.setBadgeText({ tabId, text: kept.length ? String(kept.length) : "" });
  chrome.runtime.sendMessage?.({ type: "mediaCaptured", tabId })?.catch?.(() => {});
}

function rememberMedia(details, value) {
  const tabKey = (typeof details?.tabId === "number" && details.tabId >= 0) ? details.tabId : "global";
  const previous = mediaWriteQueues.get(tabKey) || Promise.resolve();
  const operation = previous.catch(() => {}).then(() => storeMedia(details, value));
  mediaWriteQueues.set(tabKey, operation);
  return operation.finally(() => {
    if (mediaWriteQueues.get(tabKey) === operation) mediaWriteQueues.delete(tabKey);
  });
}

function queueFrameUnload(tabId, frameToken, frameId) {
  const tabKey = (typeof tabId === "number" && tabId >= 0) ? tabId : "global";
  const previous = mediaWriteQueues.get(tabKey) || Promise.resolve();
  const operation = previous.catch(() => {}).then(() => pruneUnloadedFrameBlobs(tabId, frameToken, frameId));
  mediaWriteQueues.set(tabKey, operation);
  return operation.finally(() => {
    if (mediaWriteQueues.get(tabKey) === operation) mediaWriteQueues.delete(tabKey);
  });
}

function matchesRfc6265(cookie, targetUrlStr) {
  try {
    const targetUrl = new URL(targetUrlStr);
    const targetHost = targetUrl.hostname.toLowerCase();
    const targetPath = targetUrl.pathname || "/";
    const isSecure = targetUrl.protocol === "https:";

    if (cookie.secure && !isSecure) {
      return false;
    }

    if (cookie.expirationDate && cookie.expirationDate * 1000 < Date.now()) {
      return false;
    }

    let cookieDomain = (cookie.domain || "").toLowerCase();
    if (cookieDomain.startsWith(".")) {
      cookieDomain = cookieDomain.slice(1);
    }
    if (!cookieDomain) return false;

    if (cookie.hostOnly) {
      if (targetHost !== cookieDomain) {
        return false;
      }
    } else {
      if (targetHost !== cookieDomain && !targetHost.endsWith("." + cookieDomain)) {
        return false;
      }
    }

    const cookiePath = cookie.path || "/";
    if (cookiePath !== "/" && targetPath !== cookiePath) {
      if (!targetPath.startsWith(cookiePath.endsWith("/") ? cookiePath : cookiePath + "/")) {
        return false;
      }
    }

    return true;
  } catch (_) {
    return false;
  }
}

function formatCookieRecord(c) {
  return {
    name: c.name || "",
    value: c.value || "",
    domain: c.domain || "",
    hostOnly: !!c.hostOnly,
    path: c.path || "/",
    secure: !!c.secure,
    httpOnly: !!c.httpOnly,
    session: !!c.session,
    expirationDate: typeof c.expirationDate === "number" ? Math.floor(c.expirationDate) : 0
  };
}

async function getScopedCookies(targetUrl, initialUrl = "") {
  try {
    const rawTargetCookies = await chrome.cookies.getAll({ url: targetUrl }).catch(() => []);
    const targetCookies = rawTargetCookies.filter((c) => matchesRfc6265(c, targetUrl));
    let initialCookies = [];
    if (initialUrl && initialUrl !== targetUrl) {
      const candidates = await chrome.cookies.getAll({ url: initialUrl }).catch(() => []);
      initialCookies = candidates.filter((c) => matchesRfc6265(c, targetUrl));
    }
    const cookieMap = new Map();
    const structuredList = [];
    for (const c of initialCookies) {
      if (c && c.name && !cookieMap.has(c.name)) {
        cookieMap.set(c.name, c.value || "");
        structuredList.push(formatCookieRecord(c));
      }
    }
    for (const c of targetCookies) {
      if (c && c.name) {
        cookieMap.set(c.name, c.value || "");
        const existingIdx = structuredList.findIndex((item) => item.name === c.name);
        if (existingIdx >= 0) {
          structuredList[existingIdx] = formatCookieRecord(c);
        } else {
          structuredList.push(formatCookieRecord(c));
        }
      }
    }
    const cookieHeader = Array.from(cookieMap.entries()).map(([name, val]) => `${name}=${val}`).join("; ");
    return { cookieHeader, structuredCookies: structuredList };
  } catch (_) {
    return { cookieHeader: "", structuredCookies: [] };
  }
}

async function makeDownloadEntry(url, tab, referrer = "", capturedHeaders = {}, mediaTypeHint = "", suggestedFilename = "", method = "", body = "", initialUrl = "", mimeType = "") {
  if (!/^(?:https?|ftp):/i.test(url)) {
    throw new Error("Only HTTP, HTTPS, and FTP downloads can be redirected to qtIDM.");
  }
  const { cookieHeader, structuredCookies } = await getScopedCookies(url, initialUrl);
  const request = recentRequestInfo(url);
  const headers = { ...request.headers, ...capturedHeaders };
  const pageReferrer = referrer || tab?.url || "";
  setHeaderDefault(headers, "Cookie", cookieHeader);
  setHeaderDefault(headers, "Referer", pageReferrer);
  if (mediaTypeHint === "HLS" || mediaTypeHint === "DASH") {
    setHeaderDefault(headers, "Origin", httpOrigin(pageReferrer));
  }
  setHeaderDefault(headers, "User-Agent", navigator.userAgent);
  if (mediaTypeHint) headers._qtidmMediaType = mediaTypeHint;
  if (mimeType) headers._qtidmMimeType = mimeType;
  if (tab?.title) headers._qtidmPageTitle = String(tab.title).trim();
  if (pageReferrer && /^https?:\/\//i.test(pageReferrer)) headers._qtidmRefreshTarget = pageReferrer;
  return {
    url,
    headers,
    cookies: structuredCookies,
    method: method === "POST" || request.method === "POST" ? "POST" : "",
    body: body ? textToBase64(body) : (request.bodyBase64 || textToBase64(request.body)),
    suggestedFilename: suggestedFilename ? sanitizeMediaTitle(suggestedFilename) : "",
    pageTitle: tab?.title ? String(tab.title).trim() : "",
    refreshTarget: pageReferrer && /^https?:\/\//i.test(pageReferrer) ? pageReferrer : ""
  };
}

async function sendToHost(url, tab, referrer = "", capturedHeaders = {}, mediaTypeHint = "", suggestedFilename = "", method = "", body = "", initialUrl = "", mimeType = "") {
  const response = await nativeRequest({
    downloads: [await makeDownloadEntry(url, tab, referrer, capturedHeaders, mediaTypeHint, suggestedFilename, method, body, initialUrl, mimeType)]
  });
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

async function pullServiceWorkerStream(tabId, url, suggestedFilename = "", frameId = 0, targetFrameToken = "") {
  if (typeof tabId !== "number" || tabId < 0 || !/^https?:\/\//i.test(url || "")
      || typeof chrome.tabs?.sendMessage !== "function") {
    return { ok: false, handled: false, fallback: true };
  }
  const frameOpts = (typeof frameId === "number" && frameId >= 0) ? { frameId } : undefined;
  const sendToFrame = (payload) => (frameOpts
    ? chrome.tabs.sendMessage(tabId, payload, frameOpts)
    : chrome.tabs.sendMessage(tabId, payload)).catch(() => null);

  const scope = await sendToFrame({ type: "swCheckScope", url, targetFrameToken });
  if (!scope || !scope.controlled) {
    return { ok: false, handled: false, fallback: true };
  }

  const streamId = `sw-${Date.now().toString(36)}-${++swStreamSequence}`;
  const prep = await sendToFrame({ type: "swPrepare", streamId, url, targetFrameToken });
  if (!prep || !prep.ok) {
    return { ok: false, handled: false, fallback: true };
  }

  const dispositionName = prep.disposition
    ? responseFilename({ responseHeaders: [{ name: "Content-Disposition", value: prep.disposition }] })
    : "";
  const filename = sanitizeMediaTitle(suggestedFilename || dispositionName
    || browserSuggestedFilename("", prep.url || url, prep.mime || "")) || "sw_download.bin";

  let beginRes;
  try {
    beginRes = await nativeRequest({
      type: "swStreamBegin",
      streamId,
      url: prep.url || url,
      filename,
      expectedSize: typeof prep.length === "number" ? prep.length : -1,
      mimeType: prep.mime || ""
    });
    if (!beginRes || !beginRes.ok) {
      throw new Error(beginRes?.message || "qtIDM native host rejected ServiceWorker stream.");
    }
  } catch (error) {
    await sendToFrame({ type: "swRelease", streamId, targetFrameToken });
    throw error;
  }

  let sequence = 0;
  let offset = 0;
  try {
    while (true) {
      const chunk = await sendToFrame({ type: "swPull", streamId, sequence, targetFrameToken });
      if (!chunk || !chunk.ok) {
        throw new Error(chunk?.error || "ServiceWorker ReadableStream read failed.");
      }
      if (chunk.end) {
        const finishRes = await nativeRequest({
          type: "swStreamFinish",
          streamId,
          totalBytes: offset
        });
        if (!finishRes || !finishRes.ok) {
          throw new Error(finishRes?.message || "qtIDM failed finalizing ServiceWorker stream.");
        }
        return {
          ok: true,
          handled: true,
          streamId,
          path: finishRes.path || beginRes.path || "",
          bytesWritten: offset
        };
      }
      const chunkRes = await nativeRequest({
        type: "swStreamChunk",
        streamId,
        sequence,
        offset,
        data: chunk.data || ""
      });
      if (!chunkRes || !chunkRes.ok) {
        throw new Error(chunkRes?.message || "qtIDM rejected ServiceWorker stream chunk.");
      }
      offset += typeof chunk.bytes === "number" ? chunk.bytes : 0;
      sequence += 1;
    }
  } catch (error) {
    await sendToFrame({ type: "swRelease", streamId, targetFrameToken });
    await nativeRequest({ type: "swStreamAbort", streamId }).catch(() => {});
    throw error;
  }
}

async function sendBatchToHost(urls, tab, referrer = "") {
  const uniqueUrls = [...new Set((urls || []).filter((url) => /^(?:https?|ftp):/i.test(url)))].slice(0, 100);
  if (!uniqueUrls.length) {
    throw new Error("No downloadable HTTP, HTTPS, or FTP links were found in the selection.");
  }
  const downloads = await Promise.all(uniqueUrls.map((url) => makeDownloadEntry(url, tab, referrer || tab?.url || "")));
  const response = await nativeRequest({
    downloads
  });
  if (!response || !response.ok || response.accepted !== true) {
    throw new Error(response?.message || "The qtIDM native host did not acknowledge the batch.");
  }
}

chrome.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === "qtidm-all-links" || info.menuItemId === "qtidm-selected-links") {
    const selected = info.menuItemId === "qtidm-selected-links";
    const target = typeof info.frameId === "number" && info.frameId >= 0
      ? { tabId: tab.id, frameIds: [info.frameId] }
      : { tabId: tab.id };
    chrome.scripting.executeScript({
      target,
      func: (selectionOnly) => {
        const readHref = (element) => (typeof element?.href === "string" ? element.href : element?.href?.baseVal) || "";
        const links = [];
        if (selectionOnly) {
          const selection = window.getSelection();
          for (let i = 0; selection && i < selection.rangeCount; ++i) {
            const range = selection.getRangeAt(i);
            if (range.collapsed) continue;
            const wrapper = document.createElement("div");
            wrapper.append(range.cloneContents());
            for (const element of wrapper.querySelectorAll("a[href]")) {
              links.push(readHref(element));
            }
            const ancestor = range.commonAncestorContainer?.nodeType === 1
              ? range.commonAncestorContainer
              : range.commonAncestorContainer?.parentElement;
            const enclosing = ancestor?.closest?.("a[href]");
            if (enclosing) links.push(readHref(enclosing));
          }
        } else {
          for (const element of document.querySelectorAll("a[href], img[src]")) {
            links.push(readHref(element) || element.src || "");
          }
        }
        return [...new Set(links.filter((url) => /^(?:https?|ftp):/i.test(url)))].slice(0, 100);
      },
      args: [selected]
    }).then((results) => sendBatchToHost(results.flatMap((result) => result.result || []), tab, info.frameUrl || tab?.url || "")
      .catch((error) => showError(error.message))).catch((error) => showError(error.message));
    return;
  }
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (url) {
    sendToHost(url, tab, info.frameUrl || tab?.url || "").catch((error) => showError(error.message));
  }
});

async function redirectBrowserDownload(item, url) {
  let paused = false;
  let removed = false;
  const request = recentRequestInfo(url);
  const initialUrl = (item.url && item.url !== url) ? item.url : "";
  try {
    await chrome.downloads.pause(item.id);
    paused = true;
  } catch (_) {
    // Very small downloads can finish before the pause request reaches Chrome.
  }

  try {
    if (typeof item.tabId === "number" && item.tabId >= 0 && typeof chrome.tabs?.sendMessage === "function") {
      const swResult = await pullServiceWorkerStream(
        item.tabId,
        url,
        browserSuggestedFilename(item.filename, url, item.mime || ""),
        typeof item.frameId === "number" ? item.frameId : 0
      );
      if (swResult.handled) {
        await chrome.downloads.cancel(item.id).catch(() => {});
        await chrome.downloads.removeFile(item.id).catch(() => {});
        await chrome.downloads.erase({ id: item.id }).catch(() => {});
        return;
      }
    }
    const prepared = await prepareBrowserDownload(url);
    if (prepared) {
      await chrome.downloads.cancel(item.id);
      await chrome.downloads.removeFile(item.id).catch(() => {});
      await chrome.downloads.erase({ id: item.id });
      removed = true;
    }
    await sendToHost(
      url, null, item.referrer || "", {}, "", browserSuggestedFilename(item.filename, url, item.mime || ""), request.method, request.body, initialUrl, item.mime || ""
    );
    if (!prepared) {
      await chrome.downloads.cancel(item.id).catch(() => {});
      await chrome.downloads.removeFile(item.id).catch(() => {});
      await chrome.downloads.erase({ id: item.id });
    }
  } catch (error) {
    if (removed) {
      const restore = { url: item.url || url };
      if (request.method === "POST") {
        restore.method = "POST";
        restore.body = request.body;
      }
      markRestoredDownload(restore.url);
      await chrome.downloads.download(restore).catch(() => {
        restoredDownloads.delete(restore.url);
      });
    } else if (paused) {
      await chrome.downloads.resume(item.id).catch(() => {});
    }
    throw error;
  }

}

chrome.downloads.onCreated.addListener((item) => {
  const url = item.finalUrl || item.url;
  // Some browser versions omit byExtensionId for downloads created by an
  // extension service worker. The one-shot marker also prevents a restored
  // browser download from being intercepted in an endless loop.
  if (!url || consumeRestoredDownload(item.url || url)
      || item.byExtensionId === chrome.runtime.id || !/^https?:/i.test(url)) {
    return;
  }
  settings()
    .then((value) => shouldIntercept(item, value)
      ? redirectBrowserDownload(item, url)
      : undefined)
    .catch((error) => showError(`Could not redirect the browser download: ${error.message}`));
});

chrome.webRequest.onBeforeRequest.addListener(
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

chrome.webRequest.onBeforeSendHeaders.addListener(
  (details) => {
    rememberRequestHeaders(details);
    settings().then((value) => {
      if (value.captureMedia && (isManifestUrl(details.url) || isSubtitleUrl(details.url))) {
        return rememberMedia(details, value);
      }
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame", "xmlhttprequest", "media", "other"] },
  ["requestHeaders", "extraHeaders"]
);

chrome.webRequest.onHeadersReceived.addListener(
  (details) => {
    rememberResponseFilename(details);
    settings().then((value) => {
      if (value.captureMedia && (isManifestResponse(details) || isDirectMediaResponse(details)
          || isSubtitleResponse(details))) return rememberMedia(details, value);
      return undefined;
    }).catch((error) => showError(error.message));
  },
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame", "xmlhttprequest", "media", "other"] },
  ["responseHeaders"]
);

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  (async () => {
    if (message.type === "modifierState") {
      if (sender.tab?.id >= 0) modifierByTab.set(sender.tab.id, !!message.active);
      sendResponse({ ok: true });
      return;
    }
    if (message.type === "discoveredMedia") {
      const tab = sender.tab || (await chrome.tabs?.query({ active: true, lastFocusedWindow: true }))?.[0];
      if (tab?.id >= 0 && typeof message.url === "string") {
        try {
          const parsed = new URL(message.url);
          const isBlob = parsed.protocol === "blob:";
          let validOrigin = false;
          if (isBlob) {
            try {
              const inner = new URL(parsed.pathname);
              validOrigin = (inner.protocol === "http:" || inner.protocol === "https:")
                && !inner.username && !inner.password && inner.hostname !== "169.254.169.254";
            } catch (_) {}
          } else {
            validOrigin = (parsed.protocol === "http:" || parsed.protocol === "https:")
              && !parsed.username && !parsed.password && parsed.hostname !== "169.254.169.254";
          }
          if (validOrigin) {
            const value = await settings();
            if (value.captureMedia) {
              const senderFrameId = typeof sender.frameId === "number" && sender.frameId >= 0
                ? sender.frameId
                : (typeof message.frameId === "number" && message.frameId >= 0 ? message.frameId : 0);
              const isSubframe = Boolean(message.isSubframe || senderFrameId > 0);
              const frameUrl = firstHttpUrl(message.frameUrl, isSubframe ? sender.url : "");
              const ref = firstHttpUrl(message.referrer, frameUrl, sender.url, tab?.url);
              await rememberMedia({
                tabId: tab.id,
                frameId: senderFrameId,
                frameUrl,
                frameToken: typeof message.frameToken === "string" ? message.frameToken : "",
                frameTitle: typeof message.frameTitle === "string" ? message.frameTitle : "",
                isSubframe,
                url: parsed.href,
                isBlob,
                size: typeof message.size === "number" ? message.size : 0,
                mimeType: message.mimeType || "",
                filename: message.filename || "",
                mediaType: message.mediaType || "",
                documentUrl: ref,
                initiator: isBlob ? (ref ? httpOrigin(ref) : "") : httpOrigin(ref),
                type: "media",
                method: "GET",
                responseHeaders: [
                  { name: "Content-Type", value: message.mimeType || (message.mediaType === "DASH" ? "application/dash+xml" : message.mediaType === "HLS" ? "application/vnd.apple.mpegurl" : "video/mp4") }
                ]
              }, value);
            }
          }
        } catch (_) {}
      }
      sendResponse({ ok: true });
      return;
    }
    if (message.type === "frameUnloaded") {
      const tabId = sender.tab?.id;
      if (typeof tabId === "number" && tabId >= 0) {
        const senderFrameId = typeof sender.frameId === "number" ? sender.frameId : -1;
        await queueFrameUnload(tabId, typeof message.frameToken === "string" ? message.frameToken : "", senderFrameId);
      }
      sendResponse({ ok: true });
      return;
    }
    if (message.type === "pullServiceWorkerStream") {
      try {
        const targetTabId = typeof message.tabId === "number" && message.tabId >= 0
          ? message.tabId
          : (sender.tab?.id ?? -1);
        const result = await pullServiceWorkerStream(
          targetTabId,
          message.url || "",
          message.filename || "",
          typeof message.frameId === "number" ? message.frameId : (sender.frameId ?? 0),
          typeof message.targetFrameToken === "string" ? message.targetFrameToken : ""
        );
        sendResponse(result);
      } catch (error) {
        showError(error.message);
        sendResponse({ ok: false, handled: false, message: error.message });
      }
      return;
    }
    if (message.type === "qtidmScheme") {
      const tab = sender.tab || await chrome.tabs.get(message.tabId);
      await sendToHost(message.url, tab, firstHttpUrl(sender.url, tab?.url));
      sendResponse({ ok: true });
      return;
    }
    if (message.type === "extractSocialMedia") {
      try {
        const tab = sender.tab || (await chrome.tabs.get(message.tabId));
        const targetUrl = message.url || tab?.url || "";
        const response = await nativeRequest({ type: "extractSocialMedia", url: targetUrl });
        sendResponse({ ok: !!response?.ok, message: response?.message });
      } catch (error) {
        showError(error.message);
        sendResponse({ ok: false, message: error.message });
      }
      return;
    }
    const key = mediaKey(message.tabId);
    if (message.type === "listMedia") {
      const pending = mediaWriteQueues.get(message.tabId);
      if (pending) await pending.catch(() => {});
      const stored = (await captureStorage.get(key))[key];
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
      await captureStorage.remove(key);
      await chrome.action.setBadgeText({ tabId: message.tabId, text: "" });
      sendResponse({ ok: true });
      return;
    }
    if (message.type === "downloadMedia") {
      const stored = (await captureStorage.get(key))[key];
      const items = Array.isArray(stored) ? stored : [];
      const item = (typeof message.url === "string")
        ? items.find((e) => e.url === message.url) || items[message.index]
        : items[message.index];
      if (!item) {
        sendResponse({ ok: false, message: "The selected media capture no longer exists." });
        return;
      }
      try {
        const tab = await chrome.tabs.get(message.tabId);
        if (typeof item.url === "string" && item.url.startsWith("blob:")) {
          const downloadFilename = mediaSuggestedFilename(item, tab) || item.suggestedFilename || "blob_download";
          let downloaded = false;
          try {
            await chrome.downloads.download({
              url: item.url,
              filename: downloadFilename,
              saveAs: false
            });
            downloaded = true;
          } catch (err) {
            let candidateUrl = item.url;
            if (tab?.id >= 0 && typeof chrome.tabs?.sendMessage === "function") {
              const frameOpts = (typeof item.frameId === "number" && item.frameId >= 0)
                ? { frameId: item.frameId }
                : undefined;
              const sendToFrame = (payload) => (frameOpts
                ? chrome.tabs.sendMessage(tab.id, payload, frameOpts)
                : chrome.tabs.sendMessage(tab.id, payload)).catch(() => null);
              const refreshRes = await sendToFrame({
                type: "refreshBlobUrl",
                url: item.url,
                targetFrameToken: item.frameToken || ""
              });
              if (refreshRes && refreshRes.url) {
                candidateUrl = refreshRes.url;
                try {
                  await chrome.downloads.download({
                    url: candidateUrl,
                    filename: downloadFilename,
                    saveAs: false
                  });
                  downloaded = true;
                } catch (_) {}
              }
              if (!downloaded) {
                const pageRes = await sendToFrame({
                  type: "downloadBlobInPage",
                  url: candidateUrl,
                  filename: downloadFilename,
                  targetFrameToken: item.frameToken || ""
                });
                if (pageRes && pageRes.ok) {
                  downloaded = true;
                }
              }
            }
            if (!downloaded) throw err;
          }
          sendResponse({ ok: true, message: "Blob download saved to Downloads." });
          return;
        }
        const effectiveReferrer = firstHttpUrl(item.referrer, item.frameUrl, tab?.url) || item.referrer || "";
        await sendToHost(item.url, tab, effectiveReferrer, item.headers, item.type, mediaSuggestedFilename(item, tab), "", "", "", item.contentType || "");
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
  modifierByTab.delete(tabId);
  captureStorage.remove(mediaKey(tabId)).catch(() => {});
});
