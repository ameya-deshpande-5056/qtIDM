#!/usr/bin/env sh
set -eu

usage() {
    cat >&2 <<EOF
Usage: $0 --mode development|release [--output DIRECTORY]

Development mode builds an unpacked-install ZIP for Chromium and an unsigned
Firefox XPI. Release mode builds a stable-ID unpacked ZIP, a private-key-signed
CRX for Linux external-extension registration, and an unlisted Mozilla-signed
XPI.

Release environment:
  QTIDM_CHROME_EXECUTABLE             Chrome/Chromium executable
  QTIDM_CHROME_EXTENSION_KEY          PEM private-key path
  QTIDM_CHROME_EXTENSION_ID           Expected 32-character extension ID
  WEB_EXT_API_KEY                     AMO API key/JWT issuer
  WEB_EXT_API_SECRET                  AMO API secret
EOF
    exit 2
}

MODE=""
OUTPUT=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode)
            [ "$#" -ge 2 ] || usage
            MODE="$2"
            shift 2
            ;;
        --output)
            [ "$#" -ge 2 ] || usage
            OUTPUT="$2"
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

case "$MODE" in
    development|release) ;;
    *) usage ;;
esac

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
. "$ROOT/packaging/common.sh"

OUTPUT="${OUTPUT:-$ROOT/browser/packages}"
case "$OUTPUT" in
    /*) ;;
    *) OUTPUT="$ROOT/$OUTPUT" ;;
esac

command -v zip >/dev/null 2>&1 || {
    echo "zip is required to package the Chromium development extension." >&2
    exit 1
}
command -v web-ext >/dev/null 2>&1 || {
    echo "web-ext is required to lint and package the Firefox extension." >&2
    exit 1
}

CHROME_VERSION="$(sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/browser/chrome/manifest.json")"
FIREFOX_VERSION="$(sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT/browser/firefox/manifest.json")"
if [ -z "$CHROME_VERSION" ] || [ "$CHROME_VERSION" != "$FIREFOX_VERSION" ]; then
    echo "Chrome and Firefox manifests must have the same non-empty version." >&2
    exit 1
fi

WORK="$(packaging_workspace qtidm-browser-packages)"
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
assert_not_home_workspace "$WORK"
mkdir -p "$OUTPUT"

web-ext lint --source-dir "$ROOT/browser/firefox"

if [ "$MODE" = "development" ]; then
    (
        cd "$ROOT/browser/chrome"
        zip -q -X -r "$WORK/qtidm-chrome.zip" . -x '*.DS_Store'
    )
    web-ext build \
        --source-dir "$ROOT/browser/firefox" \
        --artifacts-dir "$WORK" \
        --filename qtidm-firefox-unsigned.xpi \
        --overwrite-dest
    install -m 0644 "$WORK/qtidm-chrome.zip" "$OUTPUT/qtidm-chrome.zip"
    install -m 0644 "$WORK/qtidm-firefox-unsigned.xpi" "$OUTPUT/qtidm-firefox-unsigned.xpi"
    printf 'Built development browser packages for extension version %s in %s\n' "$CHROME_VERSION" "$OUTPUT"
    exit 0
fi

: "${QTIDM_CHROME_EXECUTABLE:?Set QTIDM_CHROME_EXECUTABLE for a release build.}"
: "${QTIDM_CHROME_EXTENSION_KEY:?Set QTIDM_CHROME_EXTENSION_KEY for a release build.}"
: "${QTIDM_CHROME_EXTENSION_ID:?Set QTIDM_CHROME_EXTENSION_ID for a release build.}"
: "${WEB_EXT_API_KEY:?Set WEB_EXT_API_KEY for an unlisted Firefox signing build.}"
: "${WEB_EXT_API_SECRET:?Set WEB_EXT_API_SECRET for an unlisted Firefox signing build.}"

if [ ! -x "$QTIDM_CHROME_EXECUTABLE" ]; then
    echo "Chrome executable is not executable: $QTIDM_CHROME_EXECUTABLE" >&2
    exit 1
fi

DERIVED_CHROME_ID="$(sh "$SCRIPT_DIR/chrome-extension-id.sh" "$QTIDM_CHROME_EXTENSION_KEY")"
if [ "$DERIVED_CHROME_ID" != "$QTIDM_CHROME_EXTENSION_ID" ]; then
    echo "Chrome signing key produces extension ID $DERIVED_CHROME_ID, expected $QTIDM_CHROME_EXTENSION_ID." >&2
    exit 1
fi

cp -R "$ROOT/browser/chrome" "$WORK/chrome"
CHROME_PUBLIC_KEY="$(
    openssl pkey -in "$QTIDM_CHROME_EXTENSION_KEY" -pubout -outform DER 2>/dev/null \
        | openssl base64 -A
)"
if [ -z "$CHROME_PUBLIC_KEY" ]; then
    echo "Could not derive the Chrome manifest public key." >&2
    exit 1
fi
sed '1a\
  "key": "'"$CHROME_PUBLIC_KEY"'",
' "$WORK/chrome/manifest.json" > "$WORK/chrome/manifest.json.with-key"
mv "$WORK/chrome/manifest.json.with-key" "$WORK/chrome/manifest.json"

(
    cd "$WORK/chrome"
    zip -q -X -r "$WORK/qtidm-chrome.zip" . -x '*.DS_Store'
)
"$QTIDM_CHROME_EXECUTABLE" \
    --headless=new \
    --disable-gpu \
    --no-sandbox \
    --disable-crash-reporter \
    --disable-breakpad \
    --pack-extension="$WORK/chrome" \
    --pack-extension-key="$QTIDM_CHROME_EXTENSION_KEY"

if [ ! -s "$WORK/chrome.crx" ] || [ "$(dd if="$WORK/chrome.crx" bs=1 count=4 2>/dev/null)" != "Cr24" ]; then
    echo "Chrome did not produce a valid CRX3 package." >&2
    exit 1
fi
install -m 0644 "$WORK/chrome.crx" "$OUTPUT/qtidm-chrome.crx"
install -m 0644 "$WORK/qtidm-chrome.zip" "$OUTPUT/qtidm-chrome.zip"

mkdir -p "$WORK/firefox-signed"
web-ext sign \
    --source-dir "$ROOT/browser/firefox" \
    --artifacts-dir "$WORK/firefox-signed" \
    --channel unlisted \
    --api-key "$WEB_EXT_API_KEY" \
    --api-secret "$WEB_EXT_API_SECRET"

set -- "$WORK"/firefox-signed/*.xpi
if [ "$#" -ne 1 ] || [ ! -s "$1" ]; then
    echo "AMO signing did not produce exactly one Firefox XPI." >&2
    exit 1
fi
if ! unzip -Z1 "$1" | grep -q '^META-INF/.*\.rsa$'; then
    echo "AMO returned an XPI without a Mozilla signature." >&2
    exit 1
fi
install -m 0644 "$1" "$OUTPUT/qtidm-firefox.xpi"

printf 'Built release browser packages for extension version %s in %s\n' "$CHROME_VERSION" "$OUTPUT"
