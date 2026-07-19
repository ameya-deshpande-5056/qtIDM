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
