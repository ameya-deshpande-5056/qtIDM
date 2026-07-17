# Browser Extension Publishing

qtIDM uses native messaging. Browser extension IDs must match the native host allowlist.

## Firefox

Firefox can use a stable ID before publishing.

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

## Chrome

Chrome native messaging requires the final Chrome extension ID:

```json
"allowed_origins": [
  "chrome-extension://<chrome-extension-id>/"
]
```

Chrome extension IDs are not reliable for unpacked local builds unless the manifest has a stable `"key"` value.

## Recommended Chrome Flow

1. Package `browser/chrome` as a ZIP.
2. Upload it once to the Chrome Developer Dashboard.
3. Keep it private/unlisted if the extension is not ready.
4. Copy the assigned extension ID.
5. Copy the public key from the dashboard package page.
6. Add the public key to `browser/chrome/manifest.json`:

```json
"key": "<public-key-from-dashboard>"
```

7. Replace the native host placeholder:

```json
"allowed_origins": [
  "chrome-extension://<final-chrome-extension-id>/"
]
```

8. Build qtIDM packages.
9. Publish the Chrome extension when ready.

## Release Rule

Do not publish qtIDM packages while this placeholder remains:

```text
REPLACE_WITH_EXTENSION_ID
```

Release builds should fail if the Chrome native host manifest still contains that value.

## Flatpak Boundary

Browser native messaging manifests must be installed on the host browser side. The Flatpak package does not install those host manifests because a browser cannot launch `/usr/bin/qtIDM-native-host` from inside the app sandbox.

Use the `.deb` package or a separate host-side installer for browser integration.

## Future Improvement

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
- Chrome manifest `key`: https://developer.chrome.com/docs/extensions/reference/manifest/key
- Firefox native messaging: https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_messaging
