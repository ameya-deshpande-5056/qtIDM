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
