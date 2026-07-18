#!/usr/bin/env sh
set -eu

usage() {
    cat >&2 <<EOF
Usage: $0 [--apply] RUN_NUMBER RUN_ATTEMPT

Generate a monotonically increasing Chrome/Firefox extension version from the
checked-in three-part base version and GitHub Actions run identity. With
--apply, update both browser manifests in place.
EOF
    exit 2
}

APPLY=0
if [ "${1:-}" = "--apply" ]; then
    APPLY=1
    shift
fi
[ "$#" -eq 2 ] || usage

RUN_NUMBER="$1"
RUN_ATTEMPT="$2"
case "$RUN_NUMBER" in
    "" | *[!0-9]* | 0 | 0*) usage ;;
esac
case "$RUN_ATTEMPT" in
    "" | *[!0-9]* | 0 | 0*) usage ;;
esac
if [ "$RUN_ATTEMPT" -gt 256 ]; then
    echo "RUN_ATTEMPT exceeds the 256 versions reserved for one workflow run." >&2
    exit 1
fi

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
CHROME_MANIFEST="$ROOT/browser/chrome/manifest.json"
FIREFOX_MANIFEST="$ROOT/browser/firefox/manifest.json"

manifest_version() {
    sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$1"
}

CHROME_VERSION="$(manifest_version "$CHROME_MANIFEST")"
FIREFOX_VERSION="$(manifest_version "$FIREFOX_MANIFEST")"
if [ -z "$CHROME_VERSION" ] || [ "$CHROME_VERSION" != "$FIREFOX_VERSION" ]; then
    echo "Chrome and Firefox manifests must have the same non-empty version." >&2
    exit 1
fi

OLD_IFS="$IFS"
IFS=.
set -- $CHROME_VERSION
IFS="$OLD_IFS"
if [ "$#" -ne 3 ]; then
    echo "The checked-in extension version must contain exactly three numeric components." >&2
    exit 1
fi
MAJOR="$1"
MINOR="$2"
BASE_PATCH="$3"

for COMPONENT in "$MAJOR" "$MINOR" "$BASE_PATCH"; do
    case "$COMPONENT" in
        "" | *[!0-9]*)
            echo "Invalid checked-in extension version: $CHROME_VERSION" >&2
            exit 1
            ;;
        0 | [1-9]*) ;;
        *)
            echo "Extension version components cannot have leading zeroes: $CHROME_VERSION" >&2
            exit 1
            ;;
    esac
    if [ "$COMPONENT" -gt 65535 ]; then
        echo "Extension version components cannot exceed 65535: $CHROME_VERSION" >&2
        exit 1
    fi
done

# Reserve 256 consecutive versions for every workflow run so reruns remain
# unique while the next run is always newer than every attempt of this one.
SEQUENCE=$(( (RUN_NUMBER - 1) * 256 + RUN_ATTEMPT - 1 ))
PATCH=$(( BASE_PATCH + 1 + SEQUENCE / 65536 ))
BUILD=$(( SEQUENCE % 65536 ))
if [ "$PATCH" -gt 65535 ]; then
    echo "The generated extension version exhausted Chrome's numeric version range." >&2
    echo "Increment the checked-in major or minor extension version." >&2
    exit 1
fi
VERSION="$MAJOR.$MINOR.$PATCH.$BUILD"

update_manifest() {
    MANIFEST="$1"
    TMP="$MANIFEST.tmp.$$"
    trap 'rm -f "$TMP"' EXIT HUP INT TERM
    sed 's/\("version":[[:space:]]*"\)[^"]*"/\1'"$VERSION"'"/' "$MANIFEST" > "$TMP"
    if [ "$(manifest_version "$TMP")" != "$VERSION" ]; then
        echo "Could not update extension version in $MANIFEST" >&2
        exit 1
    fi
    mv "$TMP" "$MANIFEST"
    trap - EXIT HUP INT TERM
}

if [ "$APPLY" -eq 1 ]; then
    update_manifest "$CHROME_MANIFEST"
    update_manifest "$FIREFOX_MANIFEST"
fi

printf '%s\n' "$VERSION"
