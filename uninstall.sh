#!/usr/bin/env sh
set -eu

case "$(uname -s)" in
    Linux)
        PREFIX=/usr/local
        MANIFEST="$PREFIX/share/qtidm/install_manifest.txt"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        PREFIX=$(cygpath -m "${LOCALAPPDATA:?LOCALAPPDATA is required}/Programs/qtIDM")
        MANIFEST="$PREFIX/install_manifest.txt"
        ;;
    *)
        echo "Unsupported platform." >&2
        exit 1
        ;;
esac

if [ ! -f "$MANIFEST" ]; then
    exit 0
fi

case "$(uname -s)" in
    Linux)
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
        ;;
    *)
        while IFS= read -r path; do
            case "$path" in
                "$PREFIX"/*) cmake -E rm -f "$path" ;;
                *) exit 1 ;;
            esac
        done < "$MANIFEST"
        cmake -E rm -f "$MANIFEST"
        ;;
esac
