#!/usr/bin/env sh
set -eu

PREFIX=/usr/local
MANIFEST="$PREFIX/share/qtidm/install_manifest.txt"

if [ ! -f "$MANIFEST" ]; then
    exit 0
fi

sudo sh -c '
    set -eu
    manifest=$1
    while IFS= read -r path; do
        case "$path" in
            /usr/local/*) rm -f -- "$path" ;;
            *) exit 1 ;;
        esac
    done < "$manifest"
    rm -f -- "$manifest"
' sh "$MANIFEST"
