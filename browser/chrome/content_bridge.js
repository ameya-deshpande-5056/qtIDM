// qtidm: URLs are an explicit opt-in path for pages and bookmarklets. Alt
// temporarily bypasses automatic interception, matching desktop IDM behavior.
const extApi = globalThis.browser || globalThis.chrome;

let modifierActive = false;
function updateModifier(active) {
  if (active === modifierActive) return;
  modifierActive = active;
  extApi.runtime.sendMessage({ type: "modifierState", active }).catch(() => {});
}
addEventListener("keydown", (event) => updateModifier(event.altKey), true);
addEventListener("keyup", (event) => updateModifier(event.altKey), true);
addEventListener("blur", () => updateModifier(false), true);
addEventListener("click", (event) => {
  const anchor = event.target.closest?.("a[href^='qtidm:']");
  if (!anchor) return;
  let value = anchor.href.slice("qtidm:".length);
  try {
    if (value.startsWith("//download")) value = new URL(anchor.href).searchParams.get("url") || "";
    value = decodeURIComponent(value);
  } catch (_) {}
  if (!/^(?:https?|ftp):/i.test(value)) return;
  event.preventDefault();
  event.stopImmediatePropagation();
  extApi.runtime.sendMessage({ type: "qtidmScheme", url: value }).catch(() => {});
}, true);

// Phase D: In-Page Streaming Media Detection Bridge
function dispatchDiscoveredMedia(rawUrl, hintType = "") {
  try {
    if (!rawUrl || typeof rawUrl !== "string" || rawUrl.startsWith("blob:") || rawUrl.startsWith("data:")) return;
    const parsed = new URL(rawUrl, location.href);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return;
    if (parsed.username || parsed.password) return;
    if (parsed.hostname === "169.254.169.254") return;
    let type = hintType;
    if (!type) {
      if (/\.mpd(?:$|[?#])/i.test(parsed.href)) type = "DASH";
      else if (/\.m3u8(?:$|[?#])/i.test(parsed.href)) type = "HLS";
      else if (/\.(?:mp3|m4a|aac|ogg|opus|flac|wav)(?:$|[?#])/i.test(parsed.href)) type = "Audio";
      else if (/\.(?:vtt|srt|ass|ssa|ttml)(?:$|[?#])/i.test(parsed.href)) type = "Subtitle";
      else if (/\.(?:mp4|webm|mkv|m4v|mov|avi|flv)(?:$|[?#])/i.test(parsed.href)) type = "Video";
      else return;
    }
    extApi.runtime.sendMessage({
      type: "discoveredMedia",
      url: parsed.href,
      mediaType: type,
      referrer: location.href
    }).catch(() => {});
  } catch (_) {}
}

function inspectMediaElement(element) {
  if (!element) return;
  const src = element.currentSrc || element.src;
  if (src) dispatchDiscoveredMedia(src);
  if (element.querySelectorAll) {
    for (const source of element.querySelectorAll("source")) {
      if (source.src) dispatchDiscoveredMedia(source.src);
    }
  }
}

for (const eventName of ["play", "playing", "loadedmetadata", "canplay"]) {
  document.addEventListener(eventName, (event) => {
    inspectMediaElement(event.target);
  }, true);
}

try {
  const observer = new MutationObserver((mutations) => {
    for (const mutation of mutations) {
      for (const node of mutation.addedNodes) {
        if (node.nodeType === 1) {
          if (node.tagName === "VIDEO" || node.tagName === "AUDIO") inspectMediaElement(node);
          else if (node.querySelectorAll) {
            node.querySelectorAll("video, audio").forEach(inspectMediaElement);
          }
        }
      }
      if (mutation.type === "attributes" && mutation.attributeName === "src") {
        inspectMediaElement(mutation.target);
      }
    }
  });
  const root = document.documentElement || document;
  observer.observe(root, { childList: true, subtree: true, attributes: true, attributeFilter: ["src"] });
} catch (_) {}

try {
  const script = document.createElement("script");
  script.textContent = `(${function() {
    if (window.__qtidmHookInstalled) return;
    window.__qtidmHookInstalled = true;
    const isManifest = (u) => /\\.m3u8(?:$|[?#])|\\.mpd(?:$|[?#])/i.test(String(u || ""));
    const notify = (url) => {
      try {
        window.postMessage({
          source: "qtidm-page-hook",
          type: "discoveredMedia",
          url: new URL(url, location.href).href,
          mediaType: /\\.mpd(?:$|[?#])/i.test(url) ? "DASH" : "HLS"
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
  }.toString()})();`;
  (document.head || document.documentElement || document).appendChild(script);
  script.remove();
} catch (_) {}

window.addEventListener("message", (event) => {
  if (event.source !== window || event.data?.source !== "qtidm-page-hook") return;
  if (event.data?.type === "discoveredMedia" && event.data?.url) {
    dispatchDiscoveredMedia(event.data.url, event.data.mediaType);
  }
});
