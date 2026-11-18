// qtidm: URLs are an explicit opt-in path for pages and bookmarklets. Alt
// temporarily bypasses automatic interception, matching desktop IDM behavior.
const extApi = globalThis.browser || globalThis.chrome;

let modifierActive = false;
function updateModifier(active) {
  if (active === modifierActive) return;
  modifierActive = active;
  extApi?.runtime?.sendMessage?.({ type: "modifierState", active })?.catch?.(() => {});
}
addEventListener("keydown", (event) => updateModifier(event.altKey), true);
addEventListener("keyup", (event) => updateModifier(event.altKey), true);
addEventListener("blur", () => updateModifier(false), true);

function findClosestInEvent(event, selector) {
  if (typeof event?.composedPath === "function") {
    for (const node of event.composedPath()) {
      if (node && node.nodeType === 1) {
        if (typeof node.matches === "function" && node.matches(selector)) return node;
        if (typeof node.closest === "function") {
          const match = node.closest(selector);
          if (match) return match;
        }
      }
    }
  }
  return event?.target?.closest?.(selector) || null;
}

addEventListener("click", (event) => {
  const blobAnchor = findClosestInEvent(event, "a[href^='blob:'][download]");
  if (blobAnchor) {
    const cleanUrl = String(blobAnchor.href || "").split("#")[0];
    const dlName = String(blobAnchor.getAttribute?.("download") || blobAnchor.download || "").trim();
    if (cleanUrl && dlName) {
      const existing = blobRegistry.get(cleanUrl) || { size: 0, mimeType: "", isFile: false, isMediaSource: false };
      if (!existing.isMediaSource) {
        const updated = { ...existing, fileName: dlName };
        blobRegistry.set(cleanUrl, updated);
        dispatchDiscoveredBlob(cleanUrl, updated);
      }
    }
  }
  const anchor = findClosestInEvent(event, "a[href^='qtidm:']");
  if (!anchor) return;
  let value = anchor.href.slice("qtidm:".length);
  try {
    if (value.startsWith("//download")) value = new URL(anchor.href).searchParams.get("url") || "";
    value = decodeURIComponent(value);
  } catch (_) {}
  if (!/^(?:https?|ftp):/i.test(value)) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  extApi?.runtime?.sendMessage?.({ type: "qtidmScheme", url: value })?.catch?.(() => {});
}, true);

// Phase D: In-Page Streaming Media, Blob, Shadow DOM & Cross-Origin Frame Bridge
const blobRegistry = new Map();
const pendingBlobRefreshes = new Map();
const pendingBlobDownloads = new Map();
const pendingSwRequests = new Map();
let blobDownloadSeq = 0;
let swRequestSeq = 0;
let pageHookActive = false;

const FRAME_BRIDGE_SOURCE = "qtidm-frame-bridge";
const FRAME_BRIDGE_VERSION = 1;
const MAX_TRACKED_FRAMES = 40;
const MAX_MEDIA_PER_FRAME = 25;
const ALLOWED_FRAME_MEDIA_TYPES = new Set([
  "HLS", "DASH", "Video", "Audio", "Subtitle", "Document", "Archive", "Image", "Blob", "Media"
]);

function detectIsTopFrame() {
  try {
    if (!window.top) return true;
    return window === window.top;
  } catch (_) {
    return false;
  }
}

const isTopFrame = detectIsTopFrame();
const frameToken = isTopFrame
  ? "qtidm-top"
  : `qtidm-frame-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;

const frameRegistry = new Map(); // Top window: Map<frameToken, { frameUrl, frameTitle, origin, lastSeen, media: Map }>
const frameLocalMedia = new Map(); // Child frame: Map<url, { media, relayedDirectly }>

function isValidHttpUrl(raw) {
  if (typeof raw !== "string" || !raw) return false;
  try {
    const parsed = new URL(raw);
    return (parsed.protocol === "http:" || parsed.protocol === "https:")
      && !parsed.username && !parsed.password && parsed.hostname !== "169.254.169.254";
  } catch (_) {
    return false;
  }
}

function isValidMediaOrBlobUrl(raw) {
  if (typeof raw !== "string" || !raw || raw.length > 4096) return false;
  try {
    if (raw.startsWith("blob:")) {
      const inner = raw.slice(5).split("#")[0];
      return isValidHttpUrl(inner);
    }
    return isValidHttpUrl(raw);
  } catch (_) {
    return false;
  }
}

function resolveFrameHttpUrl() {
  try {
    if (isValidHttpUrl(location.href)) return location.href;
  } catch (_) {}
  try {
    if (isValidHttpUrl(document.referrer)) return document.referrer;
  } catch (_) {}
  return "";
}

function currentFrameTitle() {
  try {
    const title = String(document.title || "").trim();
    return title.length > 200 ? title.slice(0, 200) : title;
  } catch (_) {
    return "";
  }
}

function postEnvelopeToTop(envelope) {
  try {
    const target = window.top && window.top !== window
      ? window.top
      : (window.parent && window.parent !== window ? window.parent : null);
    if (target && typeof target.postMessage === "function") {
      target.postMessage(envelope, "*");
      return true;
    }
  } catch (_) {
    try {
      if (window.parent && window.parent !== window && typeof window.parent.postMessage === "function") {
        window.parent.postMessage(envelope, "*");
        return true;
      }
    } catch (_) {}
  }
  return false;
}

function emitDiscoveredMedia(payload) {
  const frameHttpUrl = resolveFrameHttpUrl();
  const frameTitleText = currentFrameTitle();
  const safeReferrer = isValidHttpUrl(payload.referrer) ? payload.referrer : frameHttpUrl;
  const message = {
    type: "discoveredMedia",
    url: payload.url,
    mediaType: payload.mediaType,
    referrer: safeReferrer
  };
  if (payload.isBlob) {
    message.isBlob = true;
    message.size = typeof payload.size === "number" ? payload.size : 0;
    message.mimeType = payload.mimeType || "";
    message.filename = payload.filename || "";
  }
  if (!isTopFrame) {
    message.isSubframe = true;
    message.frameToken = frameToken;
    message.frameUrl = frameHttpUrl;
    message.frameTitle = frameTitleText;
  }

  let relayedDirectly = false;
  if (typeof extApi?.runtime?.sendMessage === "function") {
    relayedDirectly = true;
    try {
      extApi.runtime.sendMessage(message)?.catch?.(() => {});
    } catch (_) {
      relayedDirectly = false;
    }
  }

  if (!isTopFrame) {
    const mediaRecord = {
      url: payload.url,
      mediaType: payload.mediaType,
      size: typeof payload.size === "number" ? payload.size : 0,
      mimeType: payload.mimeType || "",
      filename: payload.filename || "",
      isBlob: Boolean(payload.isBlob),
      referrer: safeReferrer
    };
    if (frameLocalMedia.has(payload.url)) frameLocalMedia.delete(payload.url);
    frameLocalMedia.set(payload.url, { media: mediaRecord, relayedDirectly });
    while (frameLocalMedia.size > MAX_MEDIA_PER_FRAME) {
      const oldest = frameLocalMedia.keys().next().value;
      if (!oldest) break;
      frameLocalMedia.delete(oldest);
    }
    postEnvelopeToTop({
      source: FRAME_BRIDGE_SOURCE,
      version: FRAME_BRIDGE_VERSION,
      type: "frameMedia",
      frameToken,
      frameUrl: frameHttpUrl,
      frameTitle: frameTitleText,
      relayedDirectly,
      media: mediaRecord
    });
  }
}

globalThis.__qtidm_frame_bridge = {
  isTopFrame,
  frameToken,
  getFrames() {
    const list = [];
    for (const [token, info] of frameRegistry.entries()) {
      list.push({
        frameToken: token,
        frameUrl: info.frameUrl,
        frameTitle: info.frameTitle,
        origin: info.origin,
        lastSeen: info.lastSeen,
        media: Array.from(info.media.values())
      });
    }
    return list;
  },
  getAllMedia() {
    const all = [];
    for (const info of frameRegistry.values()) {
      for (const item of info.media.values()) {
        all.push({ ...item, frameUrl: info.frameUrl, frameTitle: info.frameTitle, isSubframe: true });
      }
    }
    return all;
  },
  stats() {
    let mediaCount = 0;
    for (const info of frameRegistry.values()) mediaCount += info.media.size;
    return {
      isTopFrame,
      frameCount: frameRegistry.size,
      mediaCount,
      localCount: frameLocalMedia.size
    };
  }
};

const IGNORED_BLOB_MIMES = new Set([
  "text/javascript",
  "application/javascript",
  "application/x-javascript",
  "text/css",
  "text/html",
  "application/xhtml+xml",
  "application/wasm",
  "image/svg+xml"
]);

const ARCHIVE_BLOB_MIMES = new Set([
  "application/zip",
  "application/x-zip-compressed",
  "application/x-tar",
  "application/gzip",
  "application/x-gzip",
  "application/x-7z-compressed",
  "application/vnd.rar",
  "application/x-rar-compressed"
]);

const DOCUMENT_BLOB_MIMES = new Set([
  "application/pdf",
  "application/msword",
  "application/epub+zip",
  "text/csv"
]);

function classifyBlobMediaType(metadata) {
  const rawMime = String(metadata?.mimeType || "").toLowerCase();
  const baseMime = rawMime.split(";")[0].trim();
  const size = typeof metadata?.size === "number" ? metadata.size : 0;
  const hasName = Boolean(metadata?.fileName && String(metadata.fileName).trim());

  if (IGNORED_BLOB_MIMES.has(baseMime) || baseMime.startsWith("font/") || baseMime.startsWith("application/font-")) {
    return "";
  }
  if (baseMime.startsWith("video/")) return "Video";
  if (baseMime.startsWith("audio/")) return "Audio";
  if (baseMime === "application/vnd.apple.mpegurl" || baseMime === "application/x-mpegurl" || baseMime === "audio/mpegurl") return "HLS";
  if (baseMime === "application/dash+xml") return "DASH";
  if (DOCUMENT_BLOB_MIMES.has(baseMime)
      || baseMime.startsWith("application/vnd.openxmlformats-")
      || baseMime.startsWith("application/vnd.ms-")) {
    return "Document";
  }
  if (ARCHIVE_BLOB_MIMES.has(baseMime)) return "Archive";
  if (baseMime.startsWith("image/")) {
    return (hasName || size >= 256 * 1024) ? "Image" : "";
  }
  if (baseMime === "text/plain" || baseMime === "application/json") {
    return (hasName || size >= 64 * 1024) ? "Document" : "";
  }
  if (!baseMime || baseMime === "application/octet-stream") {
    return (hasName || size >= 64 * 1024) ? "Blob" : "";
  }
  return (hasName || size >= 64 * 1024) ? "Blob" : "";
}

function dispatchDiscoveredBlob(rawUrl, metadata) {
  try {
    if (!rawUrl || typeof rawUrl !== "string" || !rawUrl.startsWith("blob:")) return;
    if (metadata?.isMediaSource) return;
    const cleanUrl = rawUrl.split("#")[0];
    const innerUrl = cleanUrl.slice(5);
    const parsed = new URL(innerUrl, location.href);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return;
    if (parsed.username || parsed.password) return;
    if (parsed.hostname === "169.254.169.254") return;

    const type = classifyBlobMediaType(metadata);
    if (!type) return;

    const mime = String(metadata?.mimeType || "").trim().toLowerCase();
    emitDiscoveredMedia({
      url: cleanUrl,
      mediaType: type,
      size: typeof metadata?.size === "number" ? metadata.size : 0,
      mimeType: mime,
      filename: metadata?.fileName || "",
      isBlob: true,
      referrer: resolveFrameHttpUrl()
    });
  } catch (_) {}
}

function dispatchDiscoveredMedia(rawUrl, hintType = "") {
  try {
    if (!rawUrl || typeof rawUrl !== "string" || rawUrl.startsWith("data:")) return;
    if (rawUrl.startsWith("blob:")) {
      const cleanUrl = rawUrl.split("#")[0];
      const meta = blobRegistry.get(cleanUrl);
      if (meta?.isMediaSource) return;
      if (meta) {
        dispatchDiscoveredBlob(cleanUrl, meta);
      } else {
        const innerUrl = cleanUrl.slice(5);
        const parsed = new URL(innerUrl, location.href);
        if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return;
        if (parsed.username || parsed.password) return;
        if (parsed.hostname === "169.254.169.254") return;
        dispatchDiscoveredBlob(cleanUrl, {
          size: 0,
          mimeType: hintType === "Audio" ? "audio/mpeg" : "video/mp4",
          fileName: "",
          isMediaSource: false
        });
      }
      return;
    }
    const parsed = new URL(rawUrl, location.href);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return;
    if (parsed.username || parsed.password) return;
    if (parsed.hostname === "169.254.169.254") return;
    let type = "";
    if (/\.mpd(?:$|[?#])/i.test(parsed.href)) type = "DASH";
    else if (/\.m3u8(?:$|[?#])/i.test(parsed.href)) type = "HLS";
    else if (hintType) type = hintType;
    else if (/\.(?:mp3|m4a|aac|ogg|opus|flac|wav)(?:$|[?#])/i.test(parsed.href)) type = "Audio";
    else if (/\.(?:vtt|srt|ass|ssa|ttml)(?:$|[?#])/i.test(parsed.href)) type = "Subtitle";
    else if (/\.(?:mp4|webm|mkv|m4v|mov|avi|flv)(?:$|[?#])/i.test(parsed.href)) type = "Video";
    else return;
    emitDiscoveredMedia({
      url: parsed.href,
      mediaType: type,
      referrer: resolveFrameHttpUrl()
    });
  } catch (_) {}
}

function inspectMediaElement(element) {
  if (!element) return;
  const tagName = (element.tagName || "").toUpperCase();
  if (tagName === "SOURCE") {
    const parentTag = (element.parentElement?.tagName || "").toUpperCase();
    const hint = parentTag === "AUDIO" ? "Audio" : "Video";
    const src = element.src || element.getAttribute?.("src") || "";
    if (src) dispatchDiscoveredMedia(src, hint);
    return;
  }
  if (tagName !== "VIDEO" && tagName !== "AUDIO") return;
  const tagHint = tagName === "AUDIO" ? "Audio" : "Video";
  const src = element.currentSrc || element.src || element.getAttribute?.("src") || "";
  if (src) dispatchDiscoveredMedia(src, tagHint);
  if (element.querySelectorAll) {
    for (const source of element.querySelectorAll("source")) {
      const sourceSrc = source.src || source.getAttribute?.("src") || "";
      if (sourceSrc) dispatchDiscoveredMedia(sourceSrc, tagHint);
    }
  }
}

// Register message & runtime listeners BEFORE injecting the page hook script so
// synchronous startup messages (pageHookReady, initial shadow scan) are never missed.
window.addEventListener("message", (event) => {
  // Cross-origin child <-> top frame bridge
  if (event.data?.source === FRAME_BRIDGE_SOURCE && event.data?.version === FRAME_BRIDGE_VERSION) {
    if (event.source === window) return;
    if (isTopFrame) {
      const token = typeof event.data.frameToken === "string" ? event.data.frameToken.trim() : "";
      if (!token || token.length > 128) return;

      if (event.data.type === "frameUnload") {
        frameRegistry.delete(token);
        return;
      }

      if (event.data.type === "frameMedia" && event.data.media && typeof event.data.media === "object") {
        const media = event.data.media;
        if (!isValidMediaOrBlobUrl(media.url)) return;
        const cleanUrl = media.url.startsWith("blob:") ? media.url.split("#")[0] : media.url;
        const mediaType = ALLOWED_FRAME_MEDIA_TYPES.has(media.mediaType) ? media.mediaType : "Media";
        const eventOrigin = (typeof event.origin === "string" && event.origin !== "null" && isValidHttpUrl(event.origin))
          ? event.origin
          : "";
        const safeFrameUrl = isValidHttpUrl(event.data.frameUrl) ? event.data.frameUrl : eventOrigin;
        const safeFrameTitle = typeof event.data.frameTitle === "string" ? event.data.frameTitle.trim().slice(0, 200) : "";
        const safeReferrer = isValidHttpUrl(media.referrer) ? media.referrer : (safeFrameUrl || resolveFrameHttpUrl());

        let frameEntry = frameRegistry.get(token);
        if (!frameEntry) {
          while (frameRegistry.size >= MAX_TRACKED_FRAMES) {
            const oldestFrame = frameRegistry.keys().next().value;
            if (!oldestFrame) break;
            frameRegistry.delete(oldestFrame);
          }
          frameEntry = {
            frameUrl: safeFrameUrl,
            frameTitle: safeFrameTitle,
            origin: eventOrigin,
            lastSeen: Date.now(),
            media: new Map()
          };
          frameRegistry.set(token, frameEntry);
        } else {
          if (safeFrameUrl) frameEntry.frameUrl = safeFrameUrl;
          if (safeFrameTitle) frameEntry.frameTitle = safeFrameTitle;
          if (eventOrigin) frameEntry.origin = eventOrigin;
          frameEntry.lastSeen = Date.now();
        }

        const normalizedItem = {
          url: cleanUrl,
          mediaType,
          size: typeof media.size === "number" && media.size > 0 && isFinite(media.size) ? media.size : 0,
          mimeType: typeof media.mimeType === "string" ? media.mimeType.trim().slice(0, 128).toLowerCase() : "",
          filename: typeof media.filename === "string" ? media.filename.trim().slice(0, 255) : "",
          isBlob: Boolean(media.isBlob || cleanUrl.startsWith("blob:")),
          referrer: safeReferrer,
          detectedAt: Date.now()
        };
        if (frameEntry.media.has(cleanUrl)) frameEntry.media.delete(cleanUrl);
        frameEntry.media.set(cleanUrl, normalizedItem);
        while (frameEntry.media.size > MAX_MEDIA_PER_FRAME) {
          const oldestUrl = frameEntry.media.keys().next().value;
          if (!oldestUrl) break;
          frameEntry.media.delete(oldestUrl);
        }

        // Only forward to extension runtime if child frame could not relay directly
        // (e.g. sandboxed/opaque iframe without extApi.runtime), avoiding duplicates.
        if (!event.data.relayedDirectly && typeof extApi?.runtime?.sendMessage === "function") {
          const relayMsg = {
            type: "discoveredMedia",
            url: normalizedItem.url,
            mediaType: normalizedItem.mediaType,
            referrer: normalizedItem.referrer,
            isSubframe: true,
            frameToken: token,
            frameUrl: frameEntry.frameUrl,
            frameTitle: frameEntry.frameTitle
          };
          if (normalizedItem.isBlob) {
            relayMsg.isBlob = true;
            relayMsg.size = normalizedItem.size;
            relayMsg.mimeType = normalizedItem.mimeType;
            relayMsg.filename = normalizedItem.filename;
          }
          try { extApi.runtime.sendMessage(relayMsg)?.catch?.(() => {}); } catch (_) {}
        }
      }
      return;
    }

    // Child frame: replay cached discoveries when top window signals readiness
    if (!isTopFrame && event.data.type === "topFrameReady") {
      const frameHttpUrl = resolveFrameHttpUrl();
      const frameTitleText = currentFrameTitle();
      for (const entry of frameLocalMedia.values()) {
        postEnvelopeToTop({
          source: FRAME_BRIDGE_SOURCE,
          version: FRAME_BRIDGE_VERSION,
          type: "frameMedia",
          frameToken,
          frameUrl: frameHttpUrl,
          frameTitle: frameTitleText,
          relayedDirectly: entry.relayedDirectly,
          media: entry.media
        });
      }
      return;
    }
    return;
  }

  if (event.source !== window || event.data?.source !== "qtidm-page-hook") return;
  if (event.data?.type === "pageHookReady") {
    pageHookActive = true;
    return;
  }
  if (event.data?.type === "discoveredMedia" && event.data?.url) {
    dispatchDiscoveredMedia(event.data.url, event.data.mediaType);
  }
  if (event.data?.type === "discoveredBlob" && event.data?.url) {
    blobRegistry.set(event.data.url, event.data);
    if (!event.data.isMediaSource) {
      dispatchDiscoveredBlob(event.data.url, event.data);
    }
  }
  if (event.data?.type === "refreshedBlob" && event.data?.origUrl) {
    const waiter = pendingBlobRefreshes.get(event.data.origUrl);
    if (waiter) {
      pendingBlobRefreshes.delete(event.data.origUrl);
      waiter(event.data.freshUrl || null);
    }
  }
  if (event.data?.type === "blobDownloadedInPage" && event.data?.requestId) {
    const waiter = pendingBlobDownloads.get(event.data.requestId);
    if (waiter) {
      pendingBlobDownloads.delete(event.data.requestId);
      waiter(Boolean(event.data.ok));
    }
  }
  if (event.data?.type === "swPullerResult" && event.data?.requestId) {
    const waiter = pendingSwRequests.get(event.data.requestId);
    if (waiter) {
      pendingSwRequests.delete(event.data.requestId);
      waiter(event.data.result || { ok: false, fallback: true });
    }
  }
});

if (isTopFrame) {
  try {
    const count = Math.min(Number(window.frames?.length) || 0, MAX_TRACKED_FRAMES);
    for (let i = 0; i < count; i++) {
      try {
        window.frames[i]?.postMessage?.({
          source: FRAME_BRIDGE_SOURCE,
          version: FRAME_BRIDGE_VERSION,
          type: "topFrameReady"
        }, "*");
      } catch (_) {}
    }
  } catch (_) {}
} else {
  addEventListener("pagehide", () => {
    postEnvelopeToTop({
      source: FRAME_BRIDGE_SOURCE,
      version: FRAME_BRIDGE_VERSION,
      type: "frameUnload",
      frameToken
    });
    try {
      extApi?.runtime?.sendMessage?.({
        type: "frameUnloaded",
        frameToken
      })?.catch?.(() => {});
    } catch (_) {}
  });
}

extApi.runtime?.onMessage?.addListener((message, sender, sendResponse) => {
  if (message.targetFrameToken && message.targetFrameToken !== frameToken) {
    return;
  }
  if (message.type === "refreshBlobUrl" && message.url) {
    const origUrl = message.url;
    const timer = setTimeout(() => {
      pendingBlobRefreshes.delete(origUrl);
      sendResponse({ ok: false, url: null });
    }, 3000);
    pendingBlobRefreshes.set(origUrl, (freshUrl) => {
      clearTimeout(timer);
      sendResponse({ ok: Boolean(freshUrl), url: freshUrl || null });
    });
    window.postMessage({
      source: "qtidm-content-bridge",
      type: "refreshBlob",
      url: origUrl
    }, "*");
    return true;
  }
  if (message.type === "downloadBlobInPage" && message.url) {
    const requestId = `dl-${++blobDownloadSeq}`;
    const timer = setTimeout(() => {
      pendingBlobDownloads.delete(requestId);
      sendResponse({ ok: false });
    }, 3000);
    pendingBlobDownloads.set(requestId, (ok) => {
      clearTimeout(timer);
      sendResponse({ ok });
    });
    window.postMessage({
      source: "qtidm-content-bridge",
      type: "downloadBlobInPage",
      requestId,
      url: message.url,
      filename: message.filename || ""
    }, "*");
    return true;
  }
  if (message.type === "swCheckScope" || message.type === "swPrepare"
      || message.type === "swPull" || message.type === "swRelease") {
    if (!pageHookActive) {
      sendResponse({ ok: false, fallback: true, controlled: false, scope: "" });
      return;
    }
    const requestId = `sw-${++swRequestSeq}`;
    const timeoutMs = (message.type === "swPrepare" || message.type === "swPull") ? 15000 : 3000;
    const timer = setTimeout(() => {
      pendingSwRequests.delete(requestId);
      sendResponse({ ok: false, fallback: true, error: "ServiceWorker stream request timed out." });
    }, timeoutMs);
    pendingSwRequests.set(requestId, (result) => {
      clearTimeout(timer);
      sendResponse(result);
    });
    window.postMessage({
      source: "qtidm-content-bridge",
      type: "swCommand",
      requestId,
      operation: message.type,
      streamId: message.streamId || "",
      url: message.url || "",
      sequence: typeof message.sequence === "number" ? message.sequence : 0
    }, "*");
    return true;
  }
});

// Isolated-world open Shadow DOM fallback (used when strict CSP blocks inline <script>)
const MAX_ISOLATED_SHADOW_DEPTH = 12;
const observedOpenShadows = new WeakSet();
const MEDIA_EVENT_NAMES = ["play", "playing", "loadedmetadata", "canplay"];
let isolatedShadowObserver = null;

function onIsolatedMediaEvent(event) {
  inspectMediaElement(event.target);
}

for (const eventName of MEDIA_EVENT_NAMES) {
  document.addEventListener(eventName, onIsolatedMediaEvent, true);
}

function enrollIsolatedOpenShadowRoot(shadowRoot, depth = 0) {
  if (!shadowRoot || depth > MAX_ISOLATED_SHADOW_DEPTH || observedOpenShadows.has(shadowRoot)) return;
  observedOpenShadows.add(shadowRoot);
  try {
    if (typeof shadowRoot.addEventListener === "function") {
      for (const eventName of MEDIA_EVENT_NAMES) {
        shadowRoot.addEventListener(eventName, onIsolatedMediaEvent, true);
      }
    }
  } catch (_) {}
  try {
    if (!isolatedShadowObserver && typeof MutationObserver === "function") {
      isolatedShadowObserver = new MutationObserver((mutations) => {
        for (const mutation of mutations) {
          if (mutation.type === "attributes" && mutation.attributeName === "src") {
            inspectMediaElement(mutation.target);
            continue;
          }
          for (const node of mutation.addedNodes || []) {
            if (node && node.nodeType === 1) {
              scanIsolatedTree(node, 0, true);
            }
          }
        }
      });
    }
    isolatedShadowObserver?.observe(shadowRoot, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ["src"]
    });
  } catch (_) {}
  scanIsolatedTree(shadowRoot, depth, true);
}

function scanIsolatedTree(root, depth = 0, includeOpenShadows = false) {
  if (!root || depth > MAX_ISOLATED_SHADOW_DEPTH) return;
  if (root.nodeType === 1) {
    inspectMediaElement(root);
    if (includeOpenShadows && root.shadowRoot) {
      enrollIsolatedOpenShadowRoot(root.shadowRoot, depth + 1);
    }
  }
  if (typeof root.querySelectorAll === "function") {
    for (const media of root.querySelectorAll("video, audio, source")) {
      inspectMediaElement(media);
    }
    if (includeOpenShadows) {
      for (const el of root.querySelectorAll("*")) {
        if (el.shadowRoot) {
          enrollIsolatedOpenShadowRoot(el.shadowRoot, depth + 1);
        }
      }
    }
  } else if (root.children) {
    for (const child of root.children) {
      scanIsolatedTree(child, depth, includeOpenShadows);
    }
  }
}

function hasOpenShadowRoot(node) {
  if (!node || node.nodeType !== 1) return false;
  if (node.shadowRoot) return true;
  if (typeof node.querySelectorAll === "function") {
    for (const el of node.querySelectorAll("*")) {
      if (el.shadowRoot) return true;
    }
  }
  return false;
}

try {
  const observer = new MutationObserver((mutations) => {
    let foundShadowInLightDom = false;
    let foundMediaInLightDom = false;
    for (const mutation of mutations) {
      for (const node of mutation.addedNodes || []) {
        if (node && node.nodeType === 1) {
          scanIsolatedTree(node, 0, !pageHookActive);
          if (pageHookActive) {
            if (!foundShadowInLightDom && hasOpenShadowRoot(node)) {
              foundShadowInLightDom = true;
            }
            if (!foundMediaInLightDom) {
              const tag = String(node.tagName || "").toUpperCase();
              if (tag === "VIDEO" || tag === "AUDIO"
                  || (typeof node.querySelector === "function" && node.querySelector("video, audio"))) {
                foundMediaInLightDom = true;
              }
            }
          }
        }
      }
      if (mutation.type === "attributes" && mutation.attributeName === "src") {
        inspectMediaElement(mutation.target);
        if (pageHookActive && !foundMediaInLightDom && mutation.target?.nodeType === 1) {
          const tag = String(mutation.target.tagName || "").toUpperCase();
          if (tag === "VIDEO" || tag === "AUDIO") foundMediaInLightDom = true;
        }
      }
    }
    if (foundShadowInLightDom) {
      try {
        window.postMessage({ source: "qtidm-content-bridge", type: "scanShadows" }, "*");
      } catch (_) {}
    }
    if (foundMediaInLightDom) {
      try {
        window.postMessage({ source: "qtidm-content-bridge", type: "sniffLightDomMedia" }, "*");
      } catch (_) {}
    }
  });
  const root = document.documentElement || document;
  observer.observe(root, { childList: true, subtree: true, attributes: true, attributeFilter: ["src"] });
} catch (_) {}

// Inject Main-World Page Hook (Manifests + Blobs + Open & Closed Shadow DOM Traversal)
try {
  const script = document.createElement("script");
  script.textContent = `(${function() {
    if (window.__qtidmHookInstalled) return;
    window.__qtidmHookInstalled = true;
    const isManifest = (u) => /\.m3u8(?:$|[?#])|\.mpd(?:$|[?#])/i.test(String(u || ""));
    const notify = (url) => {
      try {
        window.postMessage({
          source: "qtidm-page-hook",
          type: "discoveredMedia",
          url: new URL(url, location.href).href,
          mediaType: /\.mpd(?:$|[?#])/i.test(url) ? "DASH" : "HLS"
        }, "*");
      } catch (_) {}
    };

    const origFetch = window.fetch;
    if (origFetch) {
      window.fetch = function(input, init) {
        try {
          const url = (typeof input === "string") ? input : (input?.url || "");
          if (isManifest(url)) notify(url);
        } catch (_) {}
        return origFetch.apply(this, arguments);
      };
    }

    const origOpen = XMLHttpRequest.prototype.open;
    if (origOpen) {
      XMLHttpRequest.prototype.open = function(method, url) {
        try {
          if (isManifest(url)) notify(url);
        } catch (_) {}
        return origOpen.apply(this, arguments);
      };
    }

    // --- Blob & Object URL Interceptor ---
    if (!window.__qtidm_blobs) {
      const blobCache = new Map();
      let totalCachedBytes = 0;
      const MAX_BLOBS = 25;
      const MAX_TOTAL_BYTES = 100 * 1024 * 1024;
      const TEMP_URL_TTL_MS = 60 * 1000;

      function trimBlobCache() {
        while (blobCache.size > MAX_BLOBS || totalCachedBytes > MAX_TOTAL_BYTES) {
          const oldestKey = blobCache.keys().next().value;
          if (!oldestKey) break;
          const entry = blobCache.get(oldestKey);
          blobCache.delete(oldestKey);
          if (entry && typeof entry.cachedSize === "number" && entry.cachedSize > 0) {
            totalCachedBytes = Math.max(0, totalCachedBytes - entry.cachedSize);
          }
        }
      }

      function extractBlobMetadata(blob) {
        if (!blob) return null;
        let size = 0;
        let type = "";
        let name = "";
        let isFile = false;
        try {
          size = typeof blob.size === "number" ? blob.size : 0;
          type = typeof blob.type === "string" ? blob.type.trim().toLowerCase() : "";
          if (typeof File !== "undefined" && blob instanceof File) {
            name = String(blob.name || "");
            isFile = true;
          }
        } catch (_) {
          return null;
        }
        return { size, mimeType: type, fileName: name, isFile };
      }

      const origCreateObjectURL = window.URL && window.URL.createObjectURL;
      const origRevokeObjectURL = window.URL && window.URL.revokeObjectURL;
      const origAnchorClick = typeof HTMLAnchorElement !== "undefined" && HTMLAnchorElement.prototype
        ? HTMLAnchorElement.prototype.click
        : null;

      window.__qtidm_blobs = {
        put(url, blob, isMediaSource = false) {
          const cleanUrl = String(url || "").split("#")[0];
          if (!cleanUrl) return;
          const metadata = isMediaSource
            ? { size: 0, mimeType: "video/mp4", fileName: "", isFile: false }
            : extractBlobMetadata(blob);
          if (!metadata) return;

          const existing = blobCache.get(cleanUrl);
          if (existing) {
            if (typeof existing.cachedSize === "number" && existing.cachedSize > 0) {
              totalCachedBytes = Math.max(0, totalCachedBytes - existing.cachedSize);
            }
            blobCache.delete(cleanUrl);
          }

          // Never retain MediaSource objects or single Blobs larger than MAX_TOTAL_BYTES in RAM
          const shouldRetainBlob = !isMediaSource && metadata.size <= MAX_TOTAL_BYTES;
          const cachedSize = shouldRetainBlob && metadata.size > 0 ? metadata.size : 0;

          blobCache.set(cleanUrl, {
            blob: shouldRetainBlob ? blob : null,
            metadata,
            cachedSize,
            isMediaSource: !!isMediaSource,
            revoked: false,
            timestamp: Date.now()
          });
          if (cachedSize > 0) {
            totalCachedBytes += cachedSize;
          }
          trimBlobCache();

          try {
            window.postMessage({
              source: "qtidm-page-hook",
              type: "discoveredBlob",
              url: cleanUrl,
              size: metadata.size,
              mimeType: metadata.mimeType,
              fileName: metadata.fileName,
              isFile: metadata.isFile,
              isMediaSource: !!isMediaSource
            }, "*");
          } catch (_) {}
        },
        get(url) {
          const cleanUrl = String(url || "").split("#")[0];
          return blobCache.get(cleanUrl);
        },
        updateFilename(url, fileName) {
          const cleanUrl = String(url || "").split("#")[0];
          const cleanName = String(fileName || "").trim();
          if (!cleanUrl || !cleanName) return;
          const entry = blobCache.get(cleanUrl);
          if (!entry || entry.isMediaSource) return;
          entry.metadata.fileName = cleanName;
          try {
            window.postMessage({
              source: "qtidm-page-hook",
              type: "discoveredBlob",
              url: cleanUrl,
              size: entry.metadata.size,
              mimeType: entry.metadata.mimeType,
              fileName: cleanName,
              isFile: entry.metadata.isFile,
              isMediaSource: false
            }, "*");
          } catch (_) {}
        },
        remove(url) {
          const cleanUrl = String(url || "").split("#")[0];
          const entry = blobCache.get(cleanUrl);
          if (entry) {
            entry.revoked = true;
          }
        },
        refresh(url) {
          const cleanUrl = String(url || "").split("#")[0];
          const entry = blobCache.get(cleanUrl);
          if (entry && !entry.isMediaSource && entry.blob && typeof origCreateObjectURL === "function") {
            try {
              const freshUrl = origCreateObjectURL.call(URL, entry.blob);
              setTimeout(() => {
                try {
                  if (typeof origRevokeObjectURL === "function") origRevokeObjectURL.call(URL, freshUrl);
                } catch (_) {}
              }, TEMP_URL_TTL_MS);
              return freshUrl;
            } catch (_) {}
          }
          return null;
        },
        stats() {
          return { count: blobCache.size, totalCachedBytes };
        }
      };

      if (typeof origCreateObjectURL === "function") {
        const wrappedCreate = function(target) {
          const url = origCreateObjectURL.call(URL, target);
          try {
            const isMediaSource = typeof MediaSource !== "undefined" && target instanceof MediaSource;
            window.__qtidm_blobs.put(url, target, isMediaSource);
          } catch (_) {}
          return url;
        };
        try {
          Object.defineProperty(wrappedCreate, "toString", {
            value: function toString() { return "function createObjectURL() { [native code] }"; },
            writable: true,
            configurable: true
          });
        } catch (_) {}
        try { window.URL.createObjectURL = wrappedCreate; } catch (_) {}
      }

      if (typeof origRevokeObjectURL === "function") {
        const wrappedRevoke = function(url) {
          try { window.__qtidm_blobs.remove(url); } catch (_) {}
          return origRevokeObjectURL.call(URL, url);
        };
        try {
          Object.defineProperty(wrappedRevoke, "toString", {
            value: function toString() { return "function revokeObjectURL() { [native code] }"; },
            writable: true,
            configurable: true
          });
        } catch (_) {}
        try { window.URL.revokeObjectURL = wrappedRevoke; } catch (_) {}
      }

      if (typeof origAnchorClick === "function") {
        const wrappedClick = function() {
          try {
            if (!this.__qtidmInternalDownload) {
              const href = typeof this.href === "string" ? this.href : "";
              const dlName = typeof this.download === "string" ? this.download.trim() : "";
              if (href.startsWith("blob:") && dlName) {
                window.__qtidm_blobs.updateFilename(href, dlName);
              }
            }
          } catch (_) {}
          return origAnchorClick.apply(this, arguments);
        };
        try {
          Object.defineProperty(wrappedClick, "toString", {
            value: function toString() { return "function click() { [native code] }"; },
            writable: true,
            configurable: true
          });
        } catch (_) {}
        try { HTMLAnchorElement.prototype.click = wrappedClick; } catch (_) {}
      }

      window.addEventListener("message", (event) => {
        if (event.source !== window || event.data?.source !== "qtidm-content-bridge") return;
        if (event.data?.type === "refreshBlob" && event.data?.url) {
          const freshUrl = window.__qtidm_blobs ? window.__qtidm_blobs.refresh(event.data.url) : null;
          try {
            window.postMessage({
              source: "qtidm-page-hook",
              type: "refreshedBlob",
              origUrl: event.data.url,
              freshUrl: freshUrl
            }, "*");
          } catch (_) {}
        }
        if (event.data?.type === "downloadBlobInPage" && event.data?.url) {
          let ok = false;
          try {
            const origUrl = event.data.url;
            const entry = window.__qtidm_blobs ? window.__qtidm_blobs.get(origUrl) : null;
            let targetUrl = origUrl;
            if (entry && entry.revoked) {
              const refreshed = window.__qtidm_blobs.refresh(origUrl);
              if (refreshed) targetUrl = refreshed;
            }
            const a = document.createElement("a");
            a.__qtidmInternalDownload = true;
            a.href = targetUrl;
            a.download = event.data.filename || entry?.metadata?.fileName || "blob_download";
            a.style.display = "none";
            (document.body || document.documentElement).appendChild(a);
            if (typeof origAnchorClick === "function") origAnchorClick.call(a);
            else a.click();
            setTimeout(() => { try { a.remove(); } catch (_) {} }, 1000);
            ok = true;
          } catch (_) {}
          try {
            window.postMessage({
              source: "qtidm-page-hook",
              type: "blobDownloadedInPage",
              requestId: event.data.requestId,
              ok
            }, "*");
          } catch (_) {}
        }
      });
    }

    // --- ServiceWorker Stream Puller (32 KiB ReadableStream Slicer) ---
    if (!window.__qtidm_sw_puller) {
      const SW_CHUNK_SIZE = 32 * 1024;
      const MAX_SW_SESSIONS = 4;
      const swSessions = new Map();

      function swBytesToBase64(bytes) {
        const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
        let binary = "";
        for (let offset = 0; offset < view.length; offset += 0x2000) {
          binary += String.fromCharCode.apply(null, view.subarray(offset, offset + 0x2000));
        }
        return window.btoa(binary);
      }

      function swRelease(streamId) {
        const session = swSessions.get(streamId);
        if (!session) return { ok: true, streamId };
        swSessions.delete(streamId);
        try { session.abort?.abort?.(); } catch (_) {}
        try { session.reader?.cancel?.().catch?.(() => {}); } catch (_) {}
        session.reader = null;
        session.buffer = null;
        return { ok: true, streamId };
      }

      async function swCheckScope(url) {
        try {
          const sw = navigator.serviceWorker;
          if (!sw || !sw.controller || typeof sw.getRegistration !== "function") {
            return { ok: true, controlled: false, scope: "" };
          }
          const parsed = new URL(url, location.href);
          if (parsed.origin !== location.origin) {
            return { ok: true, controlled: false, scope: "" };
          }
          const reg = await sw.getRegistration(parsed.href);
          const controlled = Boolean(reg && reg.active && reg.active === sw.controller);
          return { ok: true, controlled, scope: controlled ? String(reg.scope || "") : "" };
        } catch (_) {
          return { ok: true, controlled: false, scope: "" };
        }
      }

      async function swPrepare(streamId, url) {
        if (!streamId || swSessions.has(streamId) || swSessions.size >= MAX_SW_SESSIONS) {
          return { ok: false, fallback: true, streamId, error: "Stream slot unavailable." };
        }
        const sw = navigator.serviceWorker;
        if (!sw || !sw.controller || typeof sw.getRegistration !== "function" || typeof origFetch !== "function") {
          return { ok: false, fallback: true, streamId, error: "ServiceWorker controller unavailable." };
        }
        let parsed;
        try {
          parsed = new URL(url);
        } catch (_) {
          return { ok: false, fallback: true, streamId, error: "Invalid URL." };
        }
        if (parsed.origin !== location.origin || parsed.href !== url) {
          return { ok: false, fallback: true, streamId, error: "Cross-origin ServiceWorker fetch rejected." };
        }

        const controller = sw.controller;
        const session = {
          abort: typeof AbortController === "function" ? new AbortController() : { abort() {}, signal: undefined },
          reader: null,
          buffer: null,
          offset: 0,
          busy: false,
          sequence: 0,
          controller
        };
        swSessions.set(streamId, session);

        try {
          const reg = await sw.getRegistration(parsed.href);
          if (swSessions.get(streamId) !== session) {
            return { ok: false, fallback: true, streamId, error: "Session released." };
          }
          if (!reg || !reg.active || reg.active !== controller || sw.controller !== controller) {
            swRelease(streamId);
            return { ok: false, fallback: true, streamId, error: "Route is not controlled by active ServiceWorker." };
          }
          const resp = await origFetch.call(window, parsed.href, {
            method: "GET",
            credentials: "include",
            mode: "same-origin",
            cache: "no-store",
            redirect: "error",
            signal: session.abort.signal
          });
          if (swSessions.get(streamId) !== session) {
            try { await resp?.body?.cancel?.(); } catch (_) {}
            return { ok: false, fallback: true, streamId, error: "Session released." };
          }
          if (sw.controller !== controller || !resp || resp.status !== 200 || resp.redirected
              || !resp.body || typeof resp.body.getReader !== "function") {
            try { await resp?.body?.cancel?.(); } catch (_) {}
            swRelease(streamId);
            return { ok: false, fallback: true, streamId, error: "ServiceWorker response is not streamable." };
          }
          session.reader = resp.body.getReader();
          const rawLen = resp.headers?.get?.("Content-Length") || "";
          const parsedLen = /^\d+$/.test(rawLen) ? Number(rawLen) : -1;
          return {
            ok: true,
            streamId,
            url: resp.url || parsed.href,
            mime: resp.headers?.get?.("Content-Type") || "",
            disposition: resp.headers?.get?.("Content-Disposition") || "",
            length: Number.isSafeInteger(parsedLen) && parsedLen >= 0 ? parsedLen : -1
          };
        } catch (err) {
          if (swSessions.get(streamId) === session) swRelease(streamId);
          return { ok: false, fallback: true, streamId, error: err?.message || "ServiceWorker fetch failed." };
        }
      }

      async function swPull(streamId, sequence) {
        const session = swSessions.get(streamId);
        if (!session || !session.reader || session.busy || session.sequence !== sequence) {
          swRelease(streamId);
          return { ok: false, streamId, error: "Invalid ServiceWorker stream state or sequence." };
        }
        session.busy = true;
        try {
          while (!session.buffer || session.offset >= session.buffer.byteLength) {
            const readResult = await session.reader.read();
            if (swSessions.get(streamId) !== session) {
              return { ok: false, streamId, error: "Session released." };
            }
            if (readResult.done) {
              const finalSeq = session.sequence++;
              swRelease(streamId);
              return { ok: true, streamId, sequence: finalSeq, end: true, data: "", bytes: 0 };
            }
            const chunkVal = readResult.value;
            session.buffer = chunkVal instanceof Uint8Array ? chunkVal : new Uint8Array(chunkVal);
            session.offset = 0;
          }
          const endOffset = Math.min(session.offset + SW_CHUNK_SIZE, session.buffer.byteLength);
          const slice = session.buffer.subarray(session.offset, endOffset);
          session.offset = endOffset;
          const seq = session.sequence++;
          return {
            ok: true,
            streamId,
            sequence: seq,
            end: false,
            data: swBytesToBase64(slice),
            bytes: slice.byteLength
          };
        } catch (err) {
          swRelease(streamId);
          return { ok: false, streamId, error: err?.message || "ReadableStream read error." };
        } finally {
          session.busy = false;
        }
      }

      window.__qtidm_sw_puller = {
        checkScope: swCheckScope,
        prepare: swPrepare,
        pull: swPull,
        release: swRelease,
        activeCount() { return swSessions.size; },
        chunkSize: SW_CHUNK_SIZE
      };

      window.addEventListener("pagehide", () => {
        for (const id of Array.from(swSessions.keys())) {
          swRelease(id);
        }
      });

      window.addEventListener("message", (event) => {
        if (event.source !== window || event.data?.source !== "qtidm-content-bridge") return;
        if (event.data?.type !== "swCommand" || !event.data?.requestId) return;
        const { requestId, operation, streamId, url, sequence } = event.data;
        const reply = (result) => {
          try {
            window.postMessage({
              source: "qtidm-page-hook",
              type: "swPullerResult",
              requestId,
              result
            }, "*");
          } catch (_) {}
        };
        if (operation === "swCheckScope") {
          swCheckScope(url).then(reply, () => reply({ ok: true, controlled: false, scope: "" }));
        } else if (operation === "swPrepare") {
          swPrepare(streamId, url).then(reply, () => reply({ ok: false, fallback: true, streamId }));
        } else if (operation === "swPull") {
          swPull(streamId, sequence).then(reply, () => reply({ ok: false, streamId }));
        } else if (operation === "swRelease") {
          reply(swRelease(streamId));
        }
      });
    }

    // --- Framework Component State Sniffer (React, Vue, Preact, Svelte, Angular, Player SDKs) ---
    if (!window.__qtidm_component_sniffer) {
      const MAX_ANCESTOR_DOM_HOPS = 6;
      const MAX_FIBER_RETURN_HOPS = 12;
      const MAX_OBJECT_DEPTH = 6;
      const MAX_VISITED_OBJECTS = 250;
      const MAX_ARRAY_ITEMS = 20;
      const MAX_KEYS_PER_OBJECT = 80;
      const MAX_RESULTS = 8;

      const MEDIA_PRIORITY = { HLS: 5, DASH: 4, Video: 3, Audio: 2, Subtitle: 1 };

      const BLOCKED_KEY_RE = /(?:poster|thumb|thumbnail|avatar|icon|logo|image|img|photo|picture|banner|cover|backdrop|sprite|storyboard|previewimage|share|canonical|permalink|profile|author|channel|homepage|website|login|logout|oauth|redirect|callback|analytics|beacon|telemetry|tracking|pixel|metric|ping|vast|vmap|adtag|adurl|script|worker|stylesheet|favicon)/i;

      const DEDICATED_MEDIA_KEYS = new Set([
        "hlsurl", "hlssrc", "hlssource", "hlsmanifest",
        "dashurl", "dashsrc", "dashsource", "dashmanifest",
        "manifesturl", "manifest", "manifestsrc",
        "m3u8url", "m3u8", "mpdurl", "mpd",
        "playlisturl", "masterplaylist", "masterurl",
        "streamurl", "streamsrc", "streamsource",
        "playbackurl", "playurl", "playingurl",
        "videourl", "videosrc", "videosource",
        "audiourl", "audiosrc", "audiosource",
        "mediaurl", "mediasrc", "mediasource",
        "fileurl", "contenturl", "asseturl", "asseturi",
        "mp4url", "webmurl", "downloadurl", "originalurl",
        "sourceurl", "srcurl", "subtitleurl", "captionsurl"
      ]);

      const GENERIC_URL_KEYS = new Set([
        "src", "url", "uri", "file", "href", "link", "source", "currentsrc", "path"
      ]);

      const CONTAINER_KEYS = new Set([
        "sources", "streams", "media", "video", "audio", "track", "tracks",
        "playlist", "playlists", "variants", "representations", "renditions",
        "qualities", "formats", "files", "urls", "manifestdata", "playback",
        "player", "config", "options", "currentsource", "activesource",
        "selectedsource", "item", "currentitem", "episode", "clip",
        "data", "meta", "metadata", "asset", "content", "stream", "info",
        "videoinfo", "mediainfo", "playerconfig", "playerprops",
        "streaminfo", "playbackinfo"
      ]);

      const FRAMEWORK_TRAVERSAL_KEYS = [
        "props", "pendingProps", "memoizedProps",
        "state", "memoizedState", "next", "current", "queue", "lastRenderedState",
        "stateNode", "setupState", "data", "ctx", "exposed", "attrs",
        "$props", "$data", "_props", "_data", "$attrs",
        "__c", "component", "$$", "options_", "player", "plyr", "hls", "_hls", "dashPlayer"
      ];

      const FRAMEWORK_PARENT_KEYS = ["return", "parent", "$parent", "__"];
      const FRAMEWORK_DOM_KEY_RE = /^__(?:react|preact|vue|svelte|ng|k$|v$)|^_vnode$/i;
      const PLAYER_DOM_KEYS = ["player", "plyr", "hls", "_hls", "dashPlayer"];

      const NON_MEDIA_EXT_RE = /\.(?:jpe?g|png|gif|webp|bmp|svg|ico|avif|css|js|mjs|cjs|tsx?|jsx|map|wasm|html?|xhtml|json|woff2?|ttf|otf|eot|txt)(?:$|[?#])/i;
      const HLS_EXT_RE = /\.m3u8(?:$|[?#])/i;
      const DASH_EXT_RE = /\.mpd(?:$|[?#])/i;
      const VIDEO_EXT_RE = /\.(?:mp4|webm|mkv|m4v|mov|avi|flv)(?:$|[?#])/i;
      const AUDIO_EXT_RE = /\.(?:mp3|m4a|aac|ogg|opus|flac|wav)(?:$|[?#])/i;
      const SUBTITLE_EXT_RE = /\.(?:vtt|srt|ass|ssa|ttml)(?:$|[?#])/i;

      const reportedByElement = new WeakMap();

      function normalizeKey(key) {
        return String(key || "").toLowerCase().replace(/[^a-z0-9]/g, "");
      }

      function classifyMimeHint(rawMime) {
        if (typeof rawMime !== "string" || !rawMime) return "";
        const lower = rawMime.split(";")[0].trim().toLowerCase();
        if (!lower) return "";
        if (lower.includes("mpegurl")) return "HLS";
        if (lower.includes("dash+xml")) return "DASH";
        if (lower.startsWith("video/")) return "Video";
        if (lower.startsWith("audio/")) return "Audio";
        if (lower === "text/vtt" || lower.includes("subrip") || lower.includes("ttml")
            || lower.includes("x-ssa") || lower.includes("x-ass")) {
          return "Subtitle";
        }
        return "";
      }

      function readSiblingMimeHint(obj) {
        if (!obj || typeof obj !== "object") return "";
        const fields = ["type", "mimeType", "mime", "contentType", "format"];
        for (let i = 0; i < fields.length; i++) {
          try {
            const val = obj[fields[i]];
            if (typeof val === "string") {
              const hint = classifyMimeHint(val);
              if (hint) return hint;
              const lower = val.trim().toLowerCase();
              if (lower === "hls" || lower === "m3u8") return "HLS";
              if (lower === "dash" || lower === "mpd") return "DASH";
            }
          } catch (_) {}
        }
        return "";
      }

      function resolveMediaCandidate(rawValue, allowRelaxed, keyHint, siblingMimeHint, elementTagHint) {
        if (typeof rawValue !== "string") return null;
        const trimmed = rawValue.trim();
        if (!trimmed || trimmed.length > 4096 || /[\s\r\n]/.test(trimmed)) return null;
        if (/^(?:data:|blob:|javascript:|about:|mailto:)/i.test(trimmed)) return null;
        if (!/^(?:https?:\/\/|\/\/|\/|\.\.?\/)/i.test(trimmed)) return null;

        let parsed;
        try {
          parsed = new URL(trimmed, location.href);
        } catch (_) {
          return null;
        }
        if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return null;
        if (parsed.username || parsed.password || parsed.hostname === "169.254.169.254") return null;

        const href = parsed.href;
        if (HLS_EXT_RE.test(href)) return { url: href, mediaType: "HLS" };
        if (DASH_EXT_RE.test(href)) return { url: href, mediaType: "DASH" };
        if (VIDEO_EXT_RE.test(href)) return { url: href, mediaType: "Video" };
        if (AUDIO_EXT_RE.test(href)) return { url: href, mediaType: "Audio" };
        if (SUBTITLE_EXT_RE.test(href)) return { url: href, mediaType: "Subtitle" };

        const normKey = normalizeKey(keyHint);
        const isXml = /\.xml(?:$|[?#])/i.test(href);
        if (isXml) {
          if (siblingMimeHint === "DASH" || normKey.includes("dash") || normKey.includes("mpd")) {
            return { url: href, mediaType: "DASH" };
          }
          return null;
        }
        if (NON_MEDIA_EXT_RE.test(href)) return null;

        if (siblingMimeHint) {
          if (parsed.pathname === "/" && !parsed.search) return null;
          return { url: href, mediaType: siblingMimeHint };
        }
        if (allowRelaxed) {
          if (parsed.pathname === "/" && !parsed.search) return null;
          let inferred = elementTagHint || "Video";
          if (normKey.includes("hls") || normKey.includes("m3u8")) inferred = "HLS";
          else if (normKey.includes("dash") || normKey.includes("mpd")) inferred = "DASH";
          else if (normKey.includes("audio")) inferred = "Audio";
          else if (normKey.includes("subtitle") || normKey.includes("caption")) inferred = "Subtitle";
          return { url: href, mediaType: inferred };
        }
        return null;
      }

      function isTraversableObject(val) {
        if (!val || typeof val !== "object") return false;
        if (val === window || val === document) return false;
        try {
          if (typeof val.nodeType === "number" && val.nodeType > 0) return false;
          if (typeof Blob !== "undefined" && val instanceof Blob) return false;
          if (typeof MediaSource !== "undefined" && val instanceof MediaSource) return false;
          if (val instanceof Date || val instanceof RegExp || val instanceof Error) return false;
          if (typeof ArrayBuffer !== "undefined" && (val instanceof ArrayBuffer || ArrayBuffer.isView(val))) return false;
          if (typeof Promise !== "undefined" && val instanceof Promise) return false;
        } catch (_) {
          return false;
        }
        return true;
      }

      function sniffObjectInternal(rootObj, state, depth, elementTagHint, allowBareStrings) {
        if (!rootObj || depth < 0 || state.visited.size >= MAX_VISITED_OBJECTS || state.results.length >= MAX_RESULTS * 2) {
          return;
        }

        if (typeof rootObj === "string") {
          if (allowBareStrings) {
            const match = resolveMediaCandidate(rootObj, false, "", "", elementTagHint);
            if (match) state.add(match.url, match.mediaType, "state");
          }
          return;
        }

        if (!isTraversableObject(rootObj)) return;
        if (state.visited.has(rootObj)) return;
        state.visited.add(rootObj);

        if (Array.isArray(rootObj)) {
          const limit = Math.min(rootObj.length, MAX_ARRAY_ITEMS);
          for (let i = 0; i < limit; i++) {
            if (state.visited.size >= MAX_VISITED_OBJECTS) break;
            let item;
            try { item = rootObj[i]; } catch (_) { continue; }
            if (typeof item === "string") {
              const match = resolveMediaCandidate(item, false, "", "", elementTagHint);
              if (match) state.add(match.url, match.mediaType, "array");
            } else if (depth > 0 && isTraversableObject(item)) {
              sniffObjectInternal(item, state, depth - 1, elementTagHint, allowBareStrings);
            }
          }
          return;
        }

        const siblingMime = readSiblingMimeHint(rootObj);
        let keys;
        try {
          keys = Object.keys(rootObj);
        } catch (_) {
          return;
        }

        const keyLimit = Math.min(keys.length, MAX_KEYS_PER_OBJECT);
        for (let i = 0; i < keyLimit; i++) {
          if (state.visited.size >= MAX_VISITED_OBJECTS) break;
          const key = keys[i];
          const norm = normalizeKey(key);
          if (!norm || BLOCKED_KEY_RE.test(norm)) continue;

          let val;
          try { val = rootObj[key]; } catch (_) { continue; }
          if (val == null) continue;

          if (DEDICATED_MEDIA_KEYS.has(norm)) {
            if (typeof val === "string") {
              const match = resolveMediaCandidate(val, true, key, siblingMime, elementTagHint);
              if (match) state.add(match.url, match.mediaType, key);
            } else if (depth > 0 && isTraversableObject(val)) {
              for (const boxKey of ["value", "current"]) {
                try {
                  if (typeof val[boxKey] === "string") {
                    const boxed = resolveMediaCandidate(val[boxKey], true, key, siblingMime, elementTagHint);
                    if (boxed) state.add(boxed.url, boxed.mediaType, key);
                  }
                } catch (_) {}
              }
              sniffObjectInternal(val, state, depth - 1, elementTagHint, true);
            }
          } else if (GENERIC_URL_KEYS.has(norm)) {
            if (typeof val === "string") {
              const match = resolveMediaCandidate(val, false, key, siblingMime, elementTagHint);
              if (match) state.add(match.url, match.mediaType, key);
            } else if (depth > 0 && isTraversableObject(val)) {
              sniffObjectInternal(val, state, depth - 1, elementTagHint, false);
            }
          } else if (CONTAINER_KEYS.has(norm)) {
            if (depth > 0 && isTraversableObject(val)) {
              sniffObjectInternal(val, state, depth - 1, elementTagHint, false);
            }
          } else if (allowBareStrings && typeof val === "string") {
            const match = resolveMediaCandidate(val, false, key, siblingMime, elementTagHint);
            if (match) state.add(match.url, match.mediaType, key);
          }
        }

        if (depth <= 0) return;

        for (let i = 0; i < FRAMEWORK_TRAVERSAL_KEYS.length; i++) {
          if (state.visited.size >= MAX_VISITED_OBJECTS) break;
          const fwKey = FRAMEWORK_TRAVERSAL_KEYS[i];
          let nextVal;
          try { nextVal = rootObj[fwKey]; } catch (_) { continue; }
          if (nextVal == null) continue;
          const bareStringsInState = fwKey === "memoizedState" || fwKey === "lastRenderedState"
            || fwKey === "current" || fwKey === "ctx" || fwKey === "setupState";
          if (typeof nextVal === "string") {
            if (bareStringsInState) {
              const match = resolveMediaCandidate(nextVal, false, fwKey, "", elementTagHint);
              if (match) state.add(match.url, match.mediaType, fwKey);
            }
          } else if (isTraversableObject(nextVal)) {
            // Hooks linked list (.next) should not exhaust depth prematurely
            const nextDepth = fwKey === "next" ? depth : depth - 1;
            sniffObjectInternal(nextVal, state, nextDepth, elementTagHint, bareStringsInState);
          }
        }
      }

      function walkFrameworkParents(startNode, state, elementTagHint) {
        if (!isTraversableObject(startNode)) return;
        let current = startNode;
        for (let hop = 0; hop <= MAX_FIBER_RETURN_HOPS && current; hop++) {
          if (state.visited.size >= MAX_VISITED_OBJECTS) break;
          sniffObjectInternal(current, state, MAX_OBJECT_DEPTH, elementTagHint, false);
          let nextParent = null;
          for (let i = 0; i < FRAMEWORK_PARENT_KEYS.length; i++) {
            try {
              const candidate = current[FRAMEWORK_PARENT_KEYS[i]];
              if (isTraversableObject(candidate) && !state.visited.has(candidate)) {
                nextParent = candidate;
                break;
              }
            } catch (_) {}
          }
          current = nextParent;
        }
      }

      function collectContextDomNodes(el) {
        const nodes = [];
        let cur = el;
        for (let i = 0; cur && i < MAX_ANCESTOR_DOM_HOPS; i++) {
          nodes.push(cur);
          let next = null;
          try {
            if (cur.parentElement) {
              next = cur.parentElement;
            } else if (cur.parentNode && cur.parentNode.nodeType === 11 && cur.parentNode.host) {
              next = cur.parentNode.host;
            } else if (typeof cur.getRootNode === "function") {
              const root = cur.getRootNode();
              if (root && root !== cur && root.host) next = root.host;
            }
          } catch (_) {}
          cur = next;
        }
        return nodes;
      }

      function sniffDataset(el, state, elementTagHint) {
        try {
          const ds = el.dataset;
          if (!ds || typeof ds !== "object") return;
          const keys = Object.keys(ds);
          for (let i = 0; i < Math.min(keys.length, 30); i++) {
            const key = keys[i];
            const norm = normalizeKey(key);
            if (!norm || BLOCKED_KEY_RE.test(norm)) continue;
            const val = ds[key];
            if (typeof val !== "string") continue;
            const isDedicated = DEDICATED_MEDIA_KEYS.has(norm);
            const isGeneric = GENERIC_URL_KEYS.has(norm);
            if (!isDedicated && !isGeneric) continue;
            const match = resolveMediaCandidate(val, isDedicated, key, "", elementTagHint);
            if (match) state.add(match.url, match.mediaType, `data-${key}`);
          }
        } catch (_) {}
      }

      function createSniffState() {
        const seenUrls = new Set();
        const results = [];
        return {
          visited: new Set(),
          results,
          add(url, mediaType, sourceKey) {
            if (!url || seenUrls.has(url)) return;
            seenUrls.add(url);
            results.push({ url, mediaType, sourceKey });
          }
        };
      }

      function finalizeResults(results) {
        return results
          .slice()
          .sort((a, b) => (MEDIA_PRIORITY[b.mediaType] || 0) - (MEDIA_PRIORITY[a.mediaType] || 0))
          .slice(0, MAX_RESULTS);
      }

      function sniffObject(obj, options = {}) {
        const state = createSniffState();
        const tagHint = options.tagHint || "Video";
        const depth = typeof options.depth === "number" ? options.depth : MAX_OBJECT_DEPTH;
        sniffObjectInternal(obj, state, depth, tagHint, Boolean(options.allowBareStrings));
        return finalizeResults(state.results);
      }

      function sniffElement(el) {
        if (!el || el.nodeType !== 1) return [];
        const tag = String(el.tagName || "").toUpperCase();
        const tagHint = tag === "AUDIO" ? "Audio" : "Video";
        const state = createSniffState();
        const domNodes = collectContextDomNodes(el);

        for (let n = 0; n < domNodes.length; n++) {
          if (state.visited.size >= MAX_VISITED_OBJECTS) break;
          const domNode = domNodes[n];
          sniffDataset(domNode, state, tagHint);

          let ownKeys = [];
          try { ownKeys = Object.keys(domNode); } catch (_) {}
          for (let k = 0; k < ownKeys.length; k++) {
            const propName = ownKeys[k];
            if (!FRAMEWORK_DOM_KEY_RE.test(propName)) continue;
            let fwObj;
            try { fwObj = domNode[propName]; } catch (_) { continue; }
            if (Array.isArray(fwObj)) {
              // Angular Ivy __ngContext__ LView array
              sniffObjectInternal(fwObj, state, MAX_OBJECT_DEPTH, tagHint, false);
            } else if (isTraversableObject(fwObj)) {
              walkFrameworkParents(fwObj, state, tagHint);
            }
          }

          for (let p = 0; p < PLAYER_DOM_KEYS.length; p++) {
            let playerObj;
            try { playerObj = domNode[PLAYER_DOM_KEYS[p]]; } catch (_) { continue; }
            if (isTraversableObject(playerObj)) {
              sniffObjectInternal(playerObj, state, 3, tagHint, false);
            }
          }
        }
        return finalizeResults(state.results);
      }

      function reportForElement(el) {
        if (!el || el.nodeType !== 1) return [];
        const discovered = sniffElement(el);
        if (!discovered.length) return [];
        let seen = reportedByElement.get(el);
        if (!seen) {
          seen = new Set();
          reportedByElement.set(el, seen);
        }
        const newlyReported = [];
        for (let i = 0; i < discovered.length; i++) {
          const item = discovered[i];
          const dedupKey = `${item.mediaType}|${item.url}`;
          if (seen.has(dedupKey)) continue;
          seen.add(dedupKey);
          newlyReported.push(item);
          try {
            window.postMessage({
              source: "qtidm-page-hook",
              type: "discoveredMedia",
              url: item.url,
              mediaType: item.mediaType
            }, "*");
          } catch (_) {}
        }
        return newlyReported;
      }

      function scanLightDomMedia(root) {
        const target = root || document.documentElement || document;
        if (!target) return;
        try {
          if (typeof target.querySelectorAll === "function") {
            const mediaNodes = target.querySelectorAll("video, audio");
            for (let i = 0; i < mediaNodes.length; i++) {
              reportForElement(mediaNodes[i]);
            }
          }
        } catch (_) {}
      }

      window.__qtidm_component_sniffer = {
        sniffObject,
        sniffElement,
        reportForElement,
        scanLightDomMedia
      };

      const LIGHT_MEDIA_EVENTS = ["play", "playing", "loadedmetadata", "canplay"];
      const onLightMediaEvent = (event) => {
        try {
          const target = event?.target;
          if (!target || target.nodeType !== 1) return;
          const tag = String(target.tagName || "").toUpperCase();
          if (tag === "VIDEO" || tag === "AUDIO") {
            reportForElement(target);
          }
        } catch (_) {}
      };
      try {
        if (typeof document.addEventListener === "function") {
          for (let i = 0; i < LIGHT_MEDIA_EVENTS.length; i++) {
            document.addEventListener(LIGHT_MEDIA_EVENTS[i], onLightMediaEvent, true);
          }
        }
      } catch (_) {}
    }

    // --- Shadow DOM & Web Component Traversal (Open & Closed Roots) ---
    if (!window.__qtidm_shadow) {
      const observedShadowRoots = new WeakSet();
      const MAX_SHADOW_DEPTH = 12;
      const SHADOW_MEDIA_EVENTS = ["play", "playing", "loadedmetadata", "canplay"];
      let enrolledCount = 0;

      function reportShadowMediaSrc(rawSrc, tagHint) {
        if (!rawSrc || typeof rawSrc !== "string" || rawSrc.startsWith("data:")) return;
        try {
          const url = rawSrc.startsWith("blob:")
            ? rawSrc.split("#")[0]
            : new URL(rawSrc, location.href).href;
          let mediaType = tagHint;
          if (/\.mpd(?:$|[?#])/i.test(url)) mediaType = "DASH";
          else if (/\.m3u8(?:$|[?#])/i.test(url)) mediaType = "HLS";
          window.postMessage({
            source: "qtidm-page-hook",
            type: "discoveredMedia",
            url,
            mediaType
          }, "*");
        } catch (_) {}
      }

      function inspectShadowMediaElement(el) {
        if (!el || el.nodeType !== 1) return;
        const tag = String(el.tagName || "").toUpperCase();
        if (tag === "SOURCE") {
          const parentTag = String(el.parentElement?.tagName || "").toUpperCase();
          const hint = parentTag === "AUDIO" ? "Audio" : "Video";
          const src = el.src || el.getAttribute?.("src") || "";
          if (src) reportShadowMediaSrc(src, hint);
          return;
        }
        if (tag !== "VIDEO" && tag !== "AUDIO") return;
        const hint = tag === "AUDIO" ? "Audio" : "Video";
        const src = el.currentSrc || el.src || el.getAttribute?.("src") || "";
        if (src) reportShadowMediaSrc(src, hint);
        if (typeof el.querySelectorAll === "function") {
          const sources = el.querySelectorAll("source");
          for (let i = 0; i < sources.length; i++) {
            const sourceSrc = sources[i].src || sources[i].getAttribute?.("src") || "";
            if (sourceSrc) reportShadowMediaSrc(sourceSrc, hint);
          }
        }
        try {
          if (window.__qtidm_component_sniffer) {
            window.__qtidm_component_sniffer.reportForElement(el);
          }
        } catch (_) {}
      }

      function onShadowMediaEvent(event) {
        try { inspectShadowMediaElement(event.target); } catch (_) {}
      }

      function onClosedShadowClick(event) {
        try {
          const anchor = event.target?.closest?.("a[href^='blob:'][download]");
          if (!anchor) return;
          const href = String(anchor.href || "");
          const dlName = String(anchor.getAttribute?.("download") || anchor.download || "").trim();
          if (href.startsWith("blob:") && dlName && window.__qtidm_blobs) {
            window.__qtidm_blobs.updateFilename(href, dlName);
          }
        } catch (_) {}
      }

      const shadowMutationObserver = typeof MutationObserver === "function"
        ? new MutationObserver((mutations) => {
            for (let i = 0; i < mutations.length; i++) {
              const mutation = mutations[i];
              if (mutation.type === "attributes" && mutation.attributeName === "src") {
                inspectShadowMediaElement(mutation.target);
                continue;
              }
              const added = mutation.addedNodes;
              if (!added) continue;
              for (let j = 0; j < added.length; j++) {
                const node = added[j];
                if (node && node.nodeType === 1) {
                  scanNodeInsideShadow(node, 0);
                }
              }
            }
          })
        : null;

      function enrollShadowRoot(root, depth = 0) {
        if (!root || depth > MAX_SHADOW_DEPTH || observedShadowRoots.has(root)) return false;
        observedShadowRoots.add(root);
        enrolledCount++;
        try {
          if (typeof root.addEventListener === "function") {
            for (let i = 0; i < SHADOW_MEDIA_EVENTS.length; i++) {
              root.addEventListener(SHADOW_MEDIA_EVENTS[i], onShadowMediaEvent, true);
            }
            if (root.mode === "closed") {
              root.addEventListener("click", onClosedShadowClick, true);
            }
          }
        } catch (_) {}
        try {
          if (shadowMutationObserver) {
            shadowMutationObserver.observe(root, {
              childList: true,
              subtree: true,
              attributes: true,
              attributeFilter: ["src"]
            });
          }
        } catch (_) {}
        scanShadowContent(root, depth);
        return true;
      }

      function scanNodeInsideShadow(node, depth) {
        if (!node || node.nodeType !== 1 || depth > MAX_SHADOW_DEPTH) return;
        inspectShadowMediaElement(node);
        if (node.shadowRoot) {
          enrollShadowRoot(node.shadowRoot, depth + 1);
        }
        if (typeof node.querySelectorAll === "function") {
          const media = node.querySelectorAll("video, audio, source");
          for (let i = 0; i < media.length; i++) {
            inspectShadowMediaElement(media[i]);
          }
          const descendants = node.querySelectorAll("*");
          for (let i = 0; i < descendants.length; i++) {
            if (descendants[i].shadowRoot) {
              enrollShadowRoot(descendants[i].shadowRoot, depth + 1);
            }
          }
        } else if (node.children) {
          for (let i = 0; i < node.children.length; i++) {
            scanNodeInsideShadow(node.children[i], depth);
          }
        }
      }

      function scanShadowContent(root, depth) {
        if (!root || depth > MAX_SHADOW_DEPTH) return;
        if (typeof root.querySelectorAll === "function") {
          const media = root.querySelectorAll("video, audio, source");
          for (let i = 0; i < media.length; i++) {
            inspectShadowMediaElement(media[i]);
          }
          const all = root.querySelectorAll("*");
          for (let i = 0; i < all.length; i++) {
            if (all[i].shadowRoot) {
              enrollShadowRoot(all[i].shadowRoot, depth + 1);
            }
          }
        } else if (root.children) {
          for (let i = 0; i < root.children.length; i++) {
            scanNodeInsideShadow(root.children[i], depth);
          }
        }
      }

      function scanLightDomForShadows(root, depth = 0) {
        if (!root || depth > MAX_SHADOW_DEPTH) return;
        if (root.shadowRoot) {
          enrollShadowRoot(root.shadowRoot, depth + 1);
        }
        if (typeof root.querySelectorAll === "function") {
          const all = root.querySelectorAll("*");
          for (let i = 0; i < all.length; i++) {
            if (all[i].shadowRoot) {
              enrollShadowRoot(all[i].shadowRoot, depth + 1);
            }
          }
        } else if (root.children) {
          for (let i = 0; i < root.children.length; i++) {
            scanLightDomForShadows(root.children[i], depth);
          }
        }
      }

      window.__qtidm_shadow = {
        enroll(root) { return enrollShadowRoot(root, 0); },
        scan(root) { scanLightDomForShadows(root || document.documentElement || document, 0); },
        isObserved(root) { return Boolean(root && observedShadowRoots.has(root)); },
        stats() { return { enrolledCount }; }
      };

      const origAttachShadow = typeof Element !== "undefined" && Element.prototype
        ? Element.prototype.attachShadow
        : null;
      if (typeof origAttachShadow === "function") {
        const wrappedAttachShadow = function(init) {
          const shadowRoot = origAttachShadow.apply(this, arguments);
          try {
            if (shadowRoot) enrollShadowRoot(shadowRoot, 0);
          } catch (_) {}
          return shadowRoot;
        };
        try {
          Object.defineProperty(wrappedAttachShadow, "toString", {
            value: function toString() { return "function attachShadow() { [native code] }"; },
            writable: true,
            configurable: true
          });
        } catch (_) {}
        try { Element.prototype.attachShadow = wrappedAttachShadow; } catch (_) {}
      }

      const origSetHtmlUnsafe = typeof Element !== "undefined" && Element.prototype
        ? Element.prototype.setHTMLUnsafe
        : null;
      if (typeof origSetHtmlUnsafe === "function") {
        const wrappedSetHtmlUnsafe = function(html) {
          const result = origSetHtmlUnsafe.apply(this, arguments);
          try { scanLightDomForShadows(this, 0); } catch (_) {}
          return result;
        };
        try {
          Object.defineProperty(wrappedSetHtmlUnsafe, "toString", {
            value: function toString() { return "function setHTMLUnsafe() { [native code] }"; },
            writable: true,
            configurable: true
          });
        } catch (_) {}
        try { Element.prototype.setHTMLUnsafe = wrappedSetHtmlUnsafe; } catch (_) {}
      }

      window.addEventListener("message", (event) => {
        if (event.source !== window || event.data?.source !== "qtidm-content-bridge") return;
        if (event.data?.type === "scanShadows") {
          try { scanLightDomForShadows(document.documentElement || document, 0); } catch (_) {}
        }
        if (event.data?.type === "sniffLightDomMedia") {
          try { window.__qtidm_component_sniffer?.scanLightDomMedia(document.documentElement || document); } catch (_) {}
        }
      });

      try {
        document.documentElement?.setAttribute?.("data-qtidm-hook", "1");
      } catch (_) {}
      try {
        window.postMessage({ source: "qtidm-page-hook", type: "pageHookReady" }, "*");
      } catch (_) {}

      try {
        scanLightDomForShadows(document.documentElement || document, 0);
        window.__qtidm_component_sniffer?.scanLightDomMedia(document.documentElement || document);
        if (document.readyState === "loading" && typeof document.addEventListener === "function") {
          document.addEventListener("DOMContentLoaded", () => {
            try { scanLightDomForShadows(document.documentElement || document, 0); } catch (_) {}
            try { window.__qtidm_component_sniffer?.scanLightDomMedia(document.documentElement || document); } catch (_) {}
          }, { once: true });
        }
      } catch (_) {}
    }
  }.toString()})();`;
  (document.head || document.documentElement || document).appendChild(script);
  script.remove();
  if (document.documentElement?.getAttribute?.("data-qtidm-hook") === "1") {
    pageHookActive = true;
    try { document.documentElement.removeAttribute("data-qtidm-hook"); } catch (_) {}
  }
} catch (_) {}

// Initial Light DOM scan (and open Shadow DOM scan if CSP blocked inline script)
try {
  const initialScan = () => scanIsolatedTree(document.documentElement || document, 0, !pageHookActive);
  initialScan();
  if (document.readyState === "loading" && typeof document.addEventListener === "function") {
    document.addEventListener("DOMContentLoaded", initialScan, { once: true });
  }
} catch (_) {}
