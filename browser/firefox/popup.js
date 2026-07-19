const api = globalThis.browser || globalThis.chrome;
const $ = (selector) => document.querySelector(selector);
let tab;

function notice(message, error = false) {
  $("#notice").textContent = message;
  $("#notice").className = error ? "error" : "";
}

async function activeTab() {
  return (await api.tabs.query({ active: true, currentWindow: true }))[0];
}

function showPanel(id) {
  document.querySelectorAll(".tab").forEach((item) => item.classList.toggle("active", item.dataset.panel === id));
  document.querySelectorAll(".panel").forEach((item) => item.classList.toggle("active", item.id === id));
}

function displayName(value) {
  const tail = value.split("/").pop()?.split("?")[0] || value;
  try { return decodeURIComponent(tail); } catch (_) { return tail; }
}

async function refresh() {
  tab ||= await activeTab();
  $("#pageTitle").textContent = tab.title || tab.url || "Current tab";
  const response = await api.runtime.sendMessage({ type: "listMedia", tabId: tab.id });
  const items = response?.items || [];
  $("#items").replaceChildren();
  $("#count").textContent = String(items.length);
  $("#status").textContent = `${items.length} captured item${items.length === 1 ? "" : "s"}`;
  $("#empty").hidden = items.length > 0;
  items.forEach((item, index) => {
    const row = document.createElement("article");
    row.className = "item";
    const type = document.createElement("span");
    type.className = "type";
    type.textContent = item.type || "Media";
    const details = document.createElement("div");
    details.className = "url-wrap";
    const url = document.createElement("span");
    url.className = "url";
    url.textContent = item.suggestedFilename || displayName(item.url);
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
      const result = await api.runtime.sendMessage({ type: "downloadMedia", tabId: tab.id, index });
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

Promise.all([refresh(), loadSettings()]).catch((error) => notice(error.message, true));
