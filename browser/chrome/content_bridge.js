// qtidm: URLs are an explicit opt-in path for pages and bookmarklets.  Alt
// temporarily bypasses automatic interception, matching desktop IDM behavior.
let modifierActive = false;
function updateModifier(active) {
  if (active === modifierActive) return;
  modifierActive = active;
  chrome.runtime.sendMessage({ type: "modifierState", active }).catch(() => {});
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
  chrome.runtime.sendMessage({ type: "qtidmScheme", url: value }).catch(() => {});
}, true);

// Phase D: In-Page Streaming Media Detection Bridge
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
    try {
      const parsed = new URL(String(event.data.url), location.href);
      if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return;
      if (parsed.username || parsed.password) return;
      if (parsed.hostname === "169.254.169.254") return;
      chrome.runtime.sendMessage({
        type: "discoveredMedia",
        url: parsed.href,
        mediaType: event.data.mediaType,
        referrer: location.href
      }).catch(() => {});
    } catch (_) {}
  }
});
