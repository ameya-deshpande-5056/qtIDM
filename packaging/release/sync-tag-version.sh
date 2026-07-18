#!/usr/bin/env sh
set -eu

usage() {
    cat >&2 <<EOF
Usage: $0 [--apply] vMAJOR.MINOR.PATCH

Validate a release tag against CMakeLists.txt. A matching tag is a no-op. A
higher tag can be applied to the CI checkout; an older tag is rejected.
EOF
    exit 2
}

APPLY=0
if [ "${1:-}" = "--apply" ]; then
    APPLY=1
    shift
fi
[ "$#" -eq 1 ] || usage

TAG="$1"
case "$TAG" in
    v*) TAG_VERSION="${TAG#v}" ;;
    *) usage ;;
esac

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
CMAKE_FILE="$ROOT/CMakeLists.txt"
CMAKE_VERSION="$(sed -n 's/^project(qtIDM VERSION \([^ ]*\).*/\1/p' "$CMAKE_FILE")"

validate_version() {
    VERSION="$1"
    OLD_IFS="$IFS"
    IFS=.
    set -- $VERSION
    IFS="$OLD_IFS"
    [ "$#" -eq 3 ] || return 1
    for COMPONENT in "$@"; do
        case "$COMPONENT" in
            "" | *[!0-9]* | 0*) [ "$COMPONENT" = "0" ] || return 1 ;;
        esac
    done
}

if ! validate_version "$TAG_VERSION"; then
    echo "Release tag must use vMAJOR.MINOR.PATCH with no leading zeroes: $TAG" >&2
    exit 1
fi
if [ -z "$CMAKE_VERSION" ] || ! validate_version "$CMAKE_VERSION"; then
    echo "CMakeLists.txt must contain a three-part numeric qtIDM project version." >&2
    exit 1
fi

version_component() {
    printf '%s\n' "$1" | cut -d. -f"$2"
}

compare_versions() {
    LEFT="$1"
    RIGHT="$2"
    for INDEX in 1 2 3; do
        LEFT_COMPONENT="$(version_component "$LEFT" "$INDEX")"
        RIGHT_COMPONENT="$(version_component "$RIGHT" "$INDEX")"
        if [ "$LEFT_COMPONENT" -gt "$RIGHT_COMPONENT" ]; then
            printf '%s\n' 1
            return
        fi
        if [ "$LEFT_COMPONENT" -lt "$RIGHT_COMPONENT" ]; then
            printf '%s\n' -1
            return
        fi
    done
    printf '%s\n' 0
}

COMPARISON="$(compare_versions "$TAG_VERSION" "$CMAKE_VERSION")"
if [ "$COMPARISON" -lt 0 ]; then
    echo "Release tag $TAG is older than CMakeLists.txt version $CMAKE_VERSION." >&2
    echo "Refusing to create a downgrade release." >&2
    exit 1
fi

if [ "$APPLY" -eq 1 ] && [ "$COMPARISON" -gt 0 ]; then
    TMP="$CMAKE_FILE.tmp.$$"
    trap 'rm -f "$TMP"' EXIT HUP INT TERM
    sed 's/^\(project(qtIDM VERSION \)[^ ]*/\1'"$TAG_VERSION"'/' "$CMAKE_FILE" > "$TMP"
    UPDATED_VERSION="$(sed -n 's/^project(qtIDM VERSION \([^ ]*\).*/\1/p' "$TMP")"
    if [ "$UPDATED_VERSION" != "$TAG_VERSION" ]; then
        echo "Could not update the qtIDM project version in CMakeLists.txt." >&2
        exit 1
    fi
    mv "$TMP" "$CMAKE_FILE"
    trap - EXIT HUP INT TERM
    echo "Updated CI checkout from qtIDM $CMAKE_VERSION to tagged version $TAG_VERSION." >&2
fi

printf '%s\n' "$TAG_VERSION"
