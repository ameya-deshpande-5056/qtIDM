let modifierActive = false;
function send(type, extra = {}) { browser.runtime.sendMessage({ type, tabId: null, ...extra }).catch(() => {}); }
function update(active) { if (active !== modifierActive) { modifierActive = active; send("modifierState", { active }); } }
addEventListener("keydown", (event) => update(event.altKey), true);
addEventListener("keyup", (event) => update(event.altKey), true);
addEventListener("blur", () => update(false), true);
addEventListener("click", (event) => {
  const anchor = event.target.closest?.("a[href^='qtidm:']");
  if (!anchor) return;
  let value = anchor.href.slice("qtidm:".length);
  try { if (value.startsWith("//download")) value = new URL(anchor.href).searchParams.get("url") || ""; value = decodeURIComponent(value); } catch (_) {}
  if (!/^(?:https?|ftp):/i.test(value)) return;
  event.preventDefault(); event.stopImmediatePropagation(); send("qtidmScheme", { url: value });
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
      send("discoveredMedia", {
        url: parsed.href,
        mediaType: event.data.mediaType,
        referrer: location.href
      });
    } catch (_) {}
  }
});
