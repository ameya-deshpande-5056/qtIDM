const api = globalThis.browser || globalThis.chrome;
const $ = (selector) => document.querySelector(selector);
let tab;

function notice(message, error = false) {
  $("#notice").textContent = message;
  $("#notice").className = error ? "error" : "";
}

async function activeTab() {
  const current = await api.tabs?.query({ active: true, currentWindow: true }).catch(() => []);
  if (current && current[0]) return current[0];
  const focused = await api.tabs?.query({ active: true, lastFocusedWindow: true }).catch(() => []);
  if (focused && focused[0]) return focused[0];
  const allActive = await api.tabs?.query({ active: true }).catch(() => []);
  return allActive?.[0];
}

function showPanel(id) {
  document.querySelectorAll(".tab").forEach((item) => item.classList.toggle("active", item.dataset.panel === id));
  document.querySelectorAll(".panel").forEach((item) => item.classList.toggle("active", item.id === id));
}

function displayName(value) {
  const tail = value.split("/").pop()?.split("?")[0] || value;
  try { return decodeURIComponent(tail); } catch (_) { return tail; }
}

function itemTitle(item, currentTab) {
  if (item.suggestedFilename && !/\.(?:m3u8|mpd)$/i.test(item.suggestedFilename)) {
    return item.suggestedFilename;
  }
  const pageTitle = (currentTab?.title || "")
    .replace(/\s+::\s+[^:]+$/, "")
    .replace(/\s*-\s*(?:Prime Video|YouTube|Netflix|Twitch|Vimeo|Dailymotion)$/i, "")
    .trim();
  if (pageTitle && pageTitle !== "Current tab") {
    if (item.type === "DASH" || item.type === "HLS") {
      return `${pageTitle} (${item.type} Full Stream)`;
    }
    return pageTitle;
  }
  return displayName(item.url);
}

const SOCIAL_HOSTS = [
  "youtube.com", "youtu.be",
  "bilibili.com", "b23.tv", "bili2233.cn",
  "instagram.com", "facebook.com", "fb.watch",
  "twitter.com", "x.com", "tiktok.com",
  "reddit.com", "redd.it", "vimeo.com",
  "dailymotion.com", "twitch.tv", "soundcloud.com"
];

function isSocialHost(url) {
  try {
    const host = new URL(url).hostname.toLowerCase();
    return SOCIAL_HOSTS.some((domain) => host === domain || host.endsWith("." + domain));
  } catch (_) {
    return false;
  }
}

async function refresh() {
  tab ||= await activeTab();
  $("#pageTitle").textContent = tab.title || tab.url || "Current tab";
  const response = await api.runtime.sendMessage({ type: "listMedia", tabId: tab.id });
  const items = response?.items || [];
  $("#items").replaceChildren();
  $("#count").textContent = String(items.length);
  $("#status").textContent = `${items.length} captured item${items.length === 1 ? "" : "s"}`;
  const isSocial = isSocialHost(tab?.url);
  const socialCard = $("#socialPlatform");
  if (socialCard) {
    socialCard.hidden = !isSocial;
    socialCard.style.display = isSocial ? "flex" : "none";
    if (isSocial) {
      try {
        const host = new URL(tab.url).hostname.replace(/^www\./, "");
        const siteNameEl = $("#socialSiteName");
        if (siteNameEl) siteNameEl.textContent = host;
      } catch (_) {}
    }
  }
  const showEmpty = items.length === 0 && !isSocial;
  $("#empty").hidden = !showEmpty;
  $("#empty").style.display = showEmpty ? "" : "none";
  const sortedItems = items.map((item, originalIndex) => ({ ...item, originalIndex }))
    .sort((a, b) => {
      const aStream = a.type === "DASH" || a.type === "HLS" ? 1 : 0;
      const bStream = b.type === "DASH" || b.type === "HLS" ? 1 : 0;
      if (aStream !== bStream) return bStream - aStream;
      return (b.detectedAt || 0) - (a.detectedAt || 0);
    });

  sortedItems.forEach((item) => {
    const isStream = item.type === "DASH" || item.type === "HLS";
    const row = document.createElement("article");
    row.className = isStream ? "item stream" : "item";
    const type = document.createElement("span");
    type.className = "type";
    type.textContent = item.type || "Media";
    const details = document.createElement("div");
    details.className = "url-wrap";
    const url = document.createElement("span");
    url.className = "url";
    url.textContent = itemTitle(item, tab);
    url.title = item.url;
    const host = document.createElement("span");
    host.className = "host";
    try { host.textContent = new URL(item.url).host; } catch (_) { host.textContent = item.url; }
    details.append(url, host);
    const button = document.createElement("button");
    button.className = "download";
    button.textContent = "GET";
    button.addEventListener("click", async () => {
      button.disabled = true;
      const result = await api.runtime.sendMessage({
        type: "downloadMedia",
        tabId: tab.id,
        index: item.originalIndex,
        url: item.url
      });
      button.disabled = false;
      notice(result?.ok ? "Capture sent to qtIDM." : (result?.message || "Could not contact qtIDM."), !result?.ok);
    });
    row.append(type, details, button);
    $("#items").append(row);
  });
}

async function loadSettings() {
  const response = await api.runtime.sendMessage({ type: "getSettings" });
  $("#interceptDownloads").checked = response.settings.interceptDownloads;
  $("#captureMedia").checked = response.settings.captureMedia;
  $("#excludedHosts").value = response.settings.excludedHosts || "";
  $("#excludedExtensions").value = response.settings.excludedExtensions || "";
  $("#includedExtensions").value = response.settings.includedExtensions || "";
  $("#minDownloadBytes").value = response.settings.minDownloadBytes || 0;
  $("#pauseInterception").checked = !!response.settings.pauseInterception;
  $("#bypassWhenModifier").checked = response.settings.bypassWhenModifier !== false;
}

async function saveSettings() {
  await api.runtime.sendMessage({
    type: "saveSettings",
    settings: {
      interceptDownloads: $("#interceptDownloads").checked,
      captureMedia: $("#captureMedia").checked,
      excludedHosts: $("#excludedHosts").value,
      excludedExtensions: $("#excludedExtensions").value,
      includedExtensions: $("#includedExtensions").value,
      minDownloadBytes: Number($("#minDownloadBytes").value) || 0,
      pauseInterception: $("#pauseInterception").checked,
      bypassWhenModifier: $("#bypassWhenModifier").checked
    }
  });
  notice("Integration settings saved.");
}

document.querySelectorAll(".tab").forEach((item) => item.addEventListener("click", () => showPanel(item.dataset.panel)));
$("#extractSocialBtn")?.addEventListener("click", async () => {
  tab ||= await activeTab();
  if (!tab?.url) return notice("No active tab URL found.", true);
  const btn = $("#extractSocialBtn");
  btn.disabled = true;
  notice("Sending extraction request to qtIDM...");
  let targetUrl = tab.url;
  try {
    const parsed = new URL(tab.url);
    if ((parsed.hostname === "bilibili.com" || parsed.hostname.endsWith(".bilibili.com"))
        && parsed.pathname.startsWith("/en/video/")) {
      parsed.pathname = parsed.pathname.slice(3);
      targetUrl = parsed.href;
    }
  } catch (_) {}
  const result = await api.runtime.sendMessage({
    type: "extractSocialMedia",
    tabId: tab.id,
    url: targetUrl
  });
  btn.disabled = false;
  notice(result?.ok ? "Extraction started in qtIDM." : (result?.message || "Could not contact qtIDM."), !result?.ok);
});
$("#clear").addEventListener("click", async () => {
  await api.runtime.sendMessage({ type: "clearMedia", tabId: tab.id });
  notice("Captured items cleared.");
  await refresh();
});
$("#sendManual").addEventListener("click", async () => {
  const urls = [...new Set($("#manualUrl").value.split(/\r?\n/).map((value) => value.trim()).filter(Boolean))];
  if (!urls.length) return notice("Enter at least one HTTP or HTTPS address.", true);
  if (urls.length > 100) return notice("A batch can contain at most 100 addresses.", true);
  if (urls.some((url) => !/^https?:\/\//i.test(url))) return notice("Every line must be a complete HTTP or HTTPS address.", true);
  const result = urls.length === 1
    ? await api.runtime.sendMessage({ type: "addManual", tabId: tab.id, url: urls[0] })
    : await api.runtime.sendMessage({ type: "addManualBatch", tabId: tab.id, urls });
  notice(result?.ok ? `${urls.length} URL${urls.length === 1 ? "" : "s"} sent to qtIDM.` : (result?.message || "Could not contact qtIDM."), !result?.ok);
});
$("#interceptDownloads").addEventListener("change", saveSettings);
$("#captureMedia").addEventListener("change", saveSettings);
$("#pauseInterception").addEventListener("change", saveSettings);
$("#bypassWhenModifier").addEventListener("change", saveSettings);
$("#excludeCurrentSite").addEventListener("click", async () => {
  tab ||= await activeTab();
  try {
    const host = new URL(tab.url).hostname;
    const current = $("#excludedHosts").value.trim();
    if (!current.split(/[\s,;]+/).includes(host)) $("#excludedHosts").value = [current, host].filter(Boolean).join("\n");
    await saveSettings();
    notice(`${host} excluded from interception.`);
  } catch (_) { notice("This tab does not have an excludable web address.", true); }
});
$("#saveSettings").addEventListener("click", saveSettings);

if (api.storage?.onChanged) {
  api.storage.onChanged.addListener((changes, areaName) => {
    if (tab && (areaName === "session" || areaName === "local")) {
      const key = `qtidm-media-${tab.id}`;
      if (changes[key]) refresh().catch(() => {});
    }
  });
}
if (api.runtime?.onMessage) {
  api.runtime.onMessage.addListener((msg) => {
    if (msg?.type === "mediaCaptured" && (!tab || msg.tabId === tab.id)) {
      refresh().catch(() => {});
    }
  });
}

Promise.all([refresh(), loadSettings()]).catch((error) => notice(error.message, true));
