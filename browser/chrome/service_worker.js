const hostName = "io.github.qtidm.native";

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.create({
    id: "qtidm-link",
    title: "Download with qtIDM",
    contexts: ["link", "image", "video", "audio"]
  });
});

async function sendToHost(url, tab) {
  const cookies = await chrome.cookies.getAll({ url }).catch(() => []);
  const cookieHeader = cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  chrome.runtime.sendNativeMessage(hostName, {
    url,
    headers: {
      Cookie: cookieHeader,
      Referer: tab?.url || "",
      "User-Agent": navigator.userAgent
    }
  });
}

chrome.contextMenus.onClicked.addListener((info, tab) => {
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (url) sendToHost(url, tab);
});

chrome.downloads.onCreated.addListener((item) => {
  if (item.url) sendToHost(item.url, null);
});
