const hostName = "io.qtidm.native";
const mediaKey = (tabId) => `qtidm-media-${tabId}`;
const settingsKey = "qtidm-settings";
const captureStorage = browser.storage.session || browser.storage.local;
const requestHeadersById = new Map();
const requestHeadersByUrl = new Map();
const responseFilenamesByUrl = new Map();
const mediaWriteQueues = new Map();
const modifierByTab = new Map();
const restoredDownloads = new Map();
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

function httpOrigin(value) {
  try {
    const parsed = new URL(value);
    return /^https?:$/.test(parsed.protocol) ? parsed.origin : "";
  } catch (_) {
    return "";
  }
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
      return "zip";
    case "application/x-tar":
      return "tar";
    case "application/gzip":
    case "application/x-gzip":
      return "gz";
    case "application/x-7z-compressed":
      return "7z";

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
  const initiator = details?.originUrl || details?.documentUrl || "";
  if (initiator && browser.tabs?.query) {
    try {
      const initOrigin = new URL(initiator).origin.toLowerCase();
      const tabs = await browser.tabs.query({}).catch(() => []);
      const activeMatch = tabs.find((t) => t.active && t.url && new URL(t.url).origin.toLowerCase() === initOrigin);
      if (activeMatch?.id >= 0) return activeMatch.id;
      const anyMatch = tabs.find((t) => t.url && new URL(t.url).origin.toLowerCase() === initOrigin);
      if (anyMatch?.id >= 0) return anyMatch.id;
    } catch (_) {}
  }
  if (browser.tabs?.query) {
    try {
      const activeTabs = await browser.tabs.query({ active: true, lastFocusedWindow: true }).catch(() => []);
      if (activeTabs?.[0]?.id >= 0) return activeTabs[0].id;
    } catch (_) {}
  }
  return -1;
}

async function storeMedia(details, value) {
  const tabId = await resolveTabId(details);
  if (tabId < 0 || !details.url || !/^https?:\/\//i.test(details.url) || isExcluded(details.url, value)) {
    return;
  }
  const key = mediaKey(tabId);
  const stored = (await captureStorage.get(key))[key];
  const items = Array.isArray(stored) ? stored : (stored && stored.url ? [stored] : []);
  const contentType = (details.responseHeaders || [])
    .find((header) => header.name.toLowerCase() === "content-type")?.value?.toLowerCase() || "";
  const duplicate = items.findIndex((entry) => entry.url === details.url);
  const previous = duplicate >= 0 ? items[duplicate] : null;
  const observedHeaders = details.requestHeaders
    ? forwardedRequestHeaders(details)
    : (details.requestId ? requestHeadersById.get(details.requestId)?.headers : null)
      || recentRequestHeaders(details.url)
      || {};
  const observedReferer = Object.entries(observedHeaders).find(([name]) => name.toLowerCase() === "referer")?.[1];
  const item = {
    url: details.url,
    referrer: observedReferer || details.originUrl || details.documentUrl
      || previous?.referrer || "",
    detectedAt: Date.now(),
    type: mediaType(details.url, contentType),
    contentType: contentType || previous?.contentType || "",
    headers: Object.keys(observedHeaders).length ? observedHeaders : previous?.headers || {},
    suggestedFilename: responseFilename(details) || previous?.suggestedFilename || ""
  };
  if (duplicate >= 0) items.splice(duplicate, 1);
  items.unshift(item);
  items.splice(25);
  await captureStorage.set({ [key]: items });
  await browser.browserAction.setBadgeText({ tabId, text: String(items.length) });
  await browser.browserAction.setTitle({
    tabId,
    title: "Open captured downloads in qtIDM"
  });
  browser.runtime.sendMessage({ type: "mediaCaptured", tabId }).catch(() => {});
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
    const rawTargetCookies = await browser.cookies.getAll({ url: targetUrl }).catch(() => []);
    const targetCookies = rawTargetCookies.filter((c) => matchesRfc6265(c, targetUrl));
    let initialCookies = [];
    if (initialUrl && initialUrl !== targetUrl) {
      const candidates = await browser.cookies.getAll({ url: initialUrl }).catch(() => []);
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
  const pageReferrer = referrer || (tab && tab.url ? tab.url : "");
  setHeaderDefault(headers, "Cookie", cookieHeader);
  setHeaderDefault(headers, "Referer", pageReferrer);
  if (mediaTypeHint === "HLS" || mediaTypeHint === "DASH") {
    setHeaderDefault(headers, "Origin", httpOrigin(pageReferrer));
  }
  setHeaderDefault(headers, "User-Agent", navigator.userAgent);
  if (mediaTypeHint) headers._qtidmMediaType = mediaTypeHint;
  if (mimeType) headers._qtidmMimeType = mimeType;
  return { url, headers, cookies: structuredCookies, method: method === "POST" || request.method === "POST" ? "POST" : "", body: body ? textToBase64(body) : (request.bodyBase64 || textToBase64(request.body)), suggestedFilename: suggestedFilename ? sanitizeMediaTitle(suggestedFilename) : "" };
}

async function sendToHost(url, tab, referrer = "", capturedHeaders = {}, mediaTypeHint = "", suggestedFilename = "", method = "", body = "", initialUrl = "", mimeType = "") {
  const response = await nativeRequest({ downloads: [await makeDownloadEntry(url, tab, referrer, capturedHeaders, mediaTypeHint, suggestedFilename, method, body, initialUrl, mimeType)] });
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

async function sendBatchToHost(urls, tab, referrer = "") {
  const uniqueUrls = [...new Set((urls || []).filter((url) => /^(?:https?|ftp):/i.test(url)))].slice(0, 100);
  if (!uniqueUrls.length) {
    throw new Error("No downloadable HTTP, HTTPS, or FTP links were found in the selection.");
  }
  const response = await nativeRequest({ downloads: await Promise.all(uniqueUrls.map((url) => makeDownloadEntry(url, tab, referrer || tab?.url || ""))) });
  if (!response || !response.ok || response.accepted !== true) {
    throw new Error(response?.message || "The qtIDM native host did not acknowledge the batch.");
  }
}

browser.contextMenus.onClicked.addListener((info, tab) => {
  if (info.menuItemId === "qtidm-all-links" || info.menuItemId === "qtidm-selected-links") {
    const selected = info.menuItemId === "qtidm-selected-links";
    const code = selected
      ? `(() => { const readHref = (e) => (typeof e?.href === 'string' ? e.href : e?.href?.baseVal) || ''; const links = []; const selection = window.getSelection(); for (let i = 0; selection && i < selection.rangeCount; ++i) { const range = selection.getRangeAt(i); if (range.collapsed) continue; const wrapper = document.createElement('div'); wrapper.append(range.cloneContents()); for (const e of wrapper.querySelectorAll('a[href]')) links.push(readHref(e)); const anc = range.commonAncestorContainer?.nodeType === 1 ? range.commonAncestorContainer : range.commonAncestorContainer?.parentElement; const enc = anc?.closest?.('a[href]'); if (enc) links.push(readHref(enc)); } return [...new Set(links.filter((url) => /^(?:https?|ftp):/i.test(url)))].slice(0, 100); })()`
      : `(() => [...new Set([...document.querySelectorAll('a[href],img[src]')].map((e) => (typeof e.href === 'string' ? e.href : e.href?.baseVal) || e.src || '').filter((url) => /^(?:https?|ftp):/i.test(url)))].slice(0, 100))()`;
    const options = typeof info.frameId === "number" && info.frameId >= 0 ? { frameId: info.frameId, code } : { code };
    browser.tabs.executeScript(tab.id, options)
      .then((results) => sendBatchToHost(results.flat(), tab, info.frameUrl || tab?.url || "")).catch((error) => showError(error.message));
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
      url, null, item.referrer || "", {}, "", browserSuggestedFilename(item.filename, url, item.mime || ""), request.method, request.body, initialUrl, item.mime || ""
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
      markRestoredDownload(restore.url);
      await browser.downloads.download(restore).catch(() => {
        restoredDownloads.delete(restore.url);
      });
    } else if (paused) {
      await browser.downloads.resume(item.id).catch(() => {});
    }
    throw error;
  }

}

browser.downloads.onCreated.addListener((item) => {
  const url = item.finalUrl || item.url;
  // Some browser versions omit byExtensionId for downloads created by an
  // extension background worker. The one-shot marker also prevents a restored
  // browser download from being intercepted in an endless loop.
  if (!url || consumeRestoredDownload(item.url || url)
      || item.byExtensionId === browser.runtime.id || !/^https?:/i.test(url)) {
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
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame", "xmlhttprequest", "media", "other"] },
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
  { urls: ["<all_urls>"], types: ["main_frame", "sub_frame", "xmlhttprequest", "media", "other"] },
  ["responseHeaders"]
);

browser.runtime.onMessage.addListener(async (message, sender) => {
  if (message.type === "modifierState") {
    if (sender.tab?.id >= 0) modifierByTab.set(sender.tab.id, !!message.active);
    return { ok: true };
  }
  if (message.type === "discoveredMedia") {
    const tab = sender.tab || (await browser.tabs?.query({ active: true, lastFocusedWindow: true }))?.[0];
    if (tab?.id >= 0 && typeof message.url === "string") {
      try {
        const parsed = new URL(message.url);
        if ((parsed.protocol === "http:" || parsed.protocol === "https:")
            && !parsed.username && !parsed.password && parsed.hostname !== "169.254.169.254") {
          const value = await settings();
          if (value.captureMedia) {
            const ref = message.referrer || sender.url || tab?.url || "";
            await rememberMedia({
              tabId: tab.id,
              url: parsed.href,
              documentUrl: ref,
              initiator: httpOrigin(ref),
              type: "media",
              method: "GET",
              responseHeaders: [
                { name: "Content-Type", value: message.mediaType === "DASH" ? "application/dash+xml" : message.mediaType === "HLS" ? "application/vnd.apple.mpegurl" : "video/mp4" }
              ]
            }, value);
          }
        }
      } catch (_) {}
    }
    return { ok: true };
  }
  if (message.type === "qtidmScheme") {
    const tab = sender.tab || await browser.tabs.get(message.tabId);
    await sendToHost(message.url, tab, tab.url);
    return { ok: true };
  }
  if (message.type === "extractSocialMedia") {
    try {
      const tab = sender.tab || (await browser.tabs.get(message.tabId));
      const targetUrl = message.url || tab?.url || "";
      const response = await nativeRequest({ type: "extractSocialMedia", url: targetUrl });
      return { ok: !!response?.ok, message: response?.message };
    } catch (error) {
      showError(error.message);
      return { ok: false, message: error.message };
    }
  }
  const key = mediaKey(message.tabId);
  if (message.type === "listMedia") {
    const pending = mediaWriteQueues.get(message.tabId);
    if (pending) await pending.catch(() => {});
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
    const item = (typeof message.url === "string")
      ? items.find((e) => e.url === message.url) || items[message.index]
      : items[message.index];
    if (!item) return { ok: false, message: "The selected media capture no longer exists." };
    const tab = await browser.tabs.get(message.tabId);
    try {
      await sendToHost(item.url, tab, item.referrer, item.headers, item.type, mediaSuggestedFilename(item, tab), "", "", "", item.contentType || "");
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
