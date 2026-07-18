"use strict";

const puppeteer = require("puppeteer");
const fs = require("node:fs");
const path = require("node:path");

async function main() {
  const [executablePath, extensionPath, profilePath, nativeManifestPath, fixtureUrl] =
    process.argv.slice(2);
  if (!executablePath || !extensionPath || !profilePath || !nativeManifestPath || !fixtureUrl) {
    throw new Error("expected Chrome, extension, profile, native manifest, and fixture paths");
  }

  const args = [
    "--disable-background-networking",
    "--disable-component-update",
    "--disable-default-apps",
    "--disable-sync",
    "--no-first-run",
    "--no-default-browser-check"
  ];
  if (typeof process.getuid === "function" && process.getuid() === 0) {
    args.push("--no-sandbox");
  }

  const browser = await puppeteer.launch({
    executablePath,
    headless: true,
    enableExtensions: [extensionPath],
    userDataDir: profilePath,
    args
  });
  try {
    const nativeDirectory = path.join(profilePath, "NativeMessagingHosts");
    fs.mkdirSync(nativeDirectory, { recursive: true });
    fs.copyFileSync(
      nativeManifestPath,
      path.join(nativeDirectory, "io.github.qtidm.native.json")
    );
    let extensions = await browser.extensions();
    let extension = [...extensions.values()].find((item) => item.name === "qtIDM Integration");
    for (let attempt = 0; !extension && attempt < 40; ++attempt) {
      await new Promise((resolve) => setTimeout(resolve, 250));
      extensions = await browser.extensions();
      extension = [...extensions.values()].find((item) => item.name === "qtIDM Integration");
    }
    console.log(`Loaded extensions: ${JSON.stringify([...extensions.entries()])}`);
    if (!extension) {
      throw new Error("Puppeteer did not load the qtIDM extension");
    }
    console.log(`Loaded qtIDM extension ${extension.id}`);
    const workerTarget = await browser.waitForTarget(
      (target) => target.type() === "service_worker" && target.url().includes(extension.id),
      { timeout: 30000 }
    );
    const worker = await workerTarget.worker();
    worker.on("console", (message) => console.log(`extension: ${message.text()}`));
    const page = await browser.newPage();
    await page.goto(fixtureUrl, { waitUntil: "load", timeout: 30000 });
    await new Promise((resolve) => setTimeout(resolve, 60000));
  } finally {
    await browser.close();
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exitCode = 1;
});
