# Browser Extension Packaging and Private Distribution

qtIDM uses native messaging. Browser extension IDs must match the native host allowlist.

## Firefox

Firefox uses a stable ID before signing.

Current ID:

```text
qtidm@io.github.qtidm
```

The Firefox extension manifest should contain:

```json
"browser_specific_settings": {
  "gecko": {
    "id": "qtidm@io.github.qtidm"
  }
}
```

The native host manifest must allow the same ID:

```json
"allowed_extensions": [
  "qtidm@io.github.qtidm"
]
```

Standard Firefox requires persistent extensions to be signed by Mozilla. qtIDM
uses AMO's `unlisted` self-distribution channel: Mozilla signs the XPI, but the
extension is not published in the public add-on catalogue. The returned signed
XPI is bundled with qtIDM releases and can be installed from a file.

## Chrome

Chrome native messaging requires the final Chrome extension ID:

```json
"allowed_origins": [
  "chrome-extension://<chrome-extension-id>/"
]
```

The release ID is derived from a permanent private key. The key signs each CRX,
so releases keep the same ID without using the Chrome Web Store. The private
key must never be committed or included in release artifacts.

## Recommended Chrome Flow

1. Generate `qtidm-chrome.pem` once with `openssl genrsa`.
2. Store it offline and add only its base64 encoding to the protected
   `QTIDM_CHROME_EXTENSION_KEY_B64` GitHub Actions secret.
3. Calculate its ID with
   `packaging/browser/chrome-extension-id.sh qtidm-chrome.pem`.
4. Store that ID in the `QTIDM_CHROME_EXTENSION_ID` repository variable.
5. Build each release CRX with the same key.
6. Pass the same ID to CMake so the native host allowlist matches the CRX.

## Release Rule

Tagged releases fail unless all of these values are configured:

```text
QTIDM_CHROME_EXTENSION_ID
QTIDM_CHROME_EXTENSION_KEY_B64
WEB_EXT_API_KEY
WEB_EXT_API_SECRET
```

The workflow independently derives the ID from the Chrome key and rejects a
mismatch before building application packages.

## Automated Builds

Development build:

```bash
sh packaging/browser/build-extensions.sh \
  --mode development \
  --output build/browser-packages
```

This lints the Firefox source and creates `qtidm-chrome.zip` plus
`qtidm-firefox-unsigned.xpi`. These artifacts are suitable for testing, not
normal Firefox installation.

Release build:

```bash
export QTIDM_CHROME_EXECUTABLE=/path/to/chrome
export QTIDM_CHROME_EXTENSION_KEY=/path/to/qtidm-chrome.pem
export QTIDM_CHROME_EXTENSION_ID=<key-derived-id>
export WEB_EXT_API_KEY=<AMO-API-key>
export WEB_EXT_API_SECRET=<AMO-API-secret>
sh packaging/browser/build-extensions.sh --mode release
```

This creates `browser/packages/qtidm-chrome.zip`,
`browser/packages/qtidm-chrome.crx`, and
`browser/packages/qtidm-firefox.xpi`. The ZIP contains the release public key,
so loading it unpacked retains the same extension ID as the bundled CRX and
native-host allowlist.

CMake includes the CRX and XPI in subsequent application package builds.
GitHub Actions publishes the stable-ID Chrome ZIP, developer-signed Chromium
CRX, and Firefox XPI as standalone release assets. The CRX is retained for
Chromium variants and managed Linux external-extension installation; branded
Chrome may reject direct installation because developer-signed CRX3 files lack
Chrome Web Store publisher proof (`CRX_REQUIRED_PROOF_MISSING`).

The Debian package installs an external-extension descriptor for Google Chrome,
pointing at the bundled CRX. AppImage and Flatpak contain both files but do not
modify host-browser configuration. Firefox installation remains user-driven
unless an administrator deploys an enterprise extension policy.

For a standalone Chrome installation from a GitHub release:

1. Extract `qtIDM-browser-chrome-<version>.zip` to a permanent directory.
2. Open `chrome://extensions` and enable **Developer mode**.
3. Select **Load unpacked** and choose the extracted directory.

Do not try to open or drag the CRX bundled inside an application package into
Chrome. On Linux, that CRX is intended only for the package-installed external
extension descriptor.

The standalone `qtIDM-browser-chromium-<version>.crx` is also available for
Chromium-family browsers that permit developer-signed CRX installation.
Acceptance depends on the browser, version, platform, and administrator policy;
the stable-ID ZIP remains the portable manual-install option.

## Flatpak Boundary

Browser native messaging manifests must be installed on the host browser side. The Flatpak package does not install those host manifests because a browser cannot launch `/usr/bin/qtIDM-native-host` from inside the app sandbox.

Use the `.deb` package or a separate host-side installer for browser integration.

## Automated End-to-End Tests

CTest registers real-browser tests for both extension variants:

```bash
python3 tests/browser_extension_e2e.py --browser chrome --strict
python3 tests/browser_extension_e2e.py --browser firefox --strict
```

Each test starts a private localhost page, triggers a browser download, installs
the extension into a temporary profile, and uses a temporary native host. It
asserts that the extension intercepts the download and forwards its URL,
cookie, referrer, and User-Agent through native messaging.

The Chrome test requires Chrome for Testing or Chromium plus Puppeteer, using
Puppeteer's supported `enableExtensions` automation API. Branded Chrome removed
the command-line unpacked-extension path starting with Chrome 137. The Firefox
test uses `web-ext` to load the unsigned development extension temporarily.

CI provisions both supported browsers and runs these tests in strict mode. For
ordinary local CTest runs, missing browser tooling reports a skipped test.

## Manifest Generation

The repo uses manifest templates:

```text
browser/native/io.github.qtidm.native.chrome.json.in
browser/native/io.github.qtidm.native.firefox.json.in
```

Generate final manifests with CMake variables:

```bash
cmake -S . -B build \
  -DQTIDM_CHROME_EXTENSION_ID=<chrome-extension-id> \
  -DQTIDM_FIREFOX_EXTENSION_ID=qtidm@io.github.qtidm
```

This keeps development IDs and release IDs explicit.

## References

- Chrome native messaging: https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging
- Chrome alternative installation: https://developer.chrome.com/docs/extensions/how-to/distribute/install-extensions
- Chrome Linux self-hosting: https://developer.chrome.com/docs/extensions/how-to/distribute/host-on-linux
- Chrome extension end-to-end testing: https://developer.chrome.com/docs/extensions/how-to/test/end-to-end-testing
- Firefox native messaging: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_messaging
- Firefox `web-ext`: https://extensionworkshop.com/documentation/develop/getting-started-with-web-ext/
- Firefox signing and self-distribution: https://extensionworkshop.com/documentation/publish/signing-and-distribution-overview/
