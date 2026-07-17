const hostName = "io.github.qtidm.native";

browser.contextMenus.create({
  id: "qtidm-link",
  title: "Download with qtIDM",
  contexts: ["link", "image", "video", "audio"]
});

async function sendToHost(url, tab) {
  let cookies = [];
  try {
    cookies = await browser.cookies.getAll({ url });
  } catch (_) {
    cookies = [];
  }
  const cookieHeader = cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  browser.runtime.sendNativeMessage(hostName, {
    url,
    headers: {
      Cookie: cookieHeader,
      Referer: tab && tab.url ? tab.url : "",
      "User-Agent": navigator.userAgent
    }
  });
}

browser.contextMenus.onClicked.addListener((info, tab) => {
  const url = info.linkUrl || info.srcUrl || info.pageUrl;
  if (url) sendToHost(url, tab);
});

browser.downloads.onCreated.addListener((item) => {
  if (item.url) sendToHost(item.url, null);
});
