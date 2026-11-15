#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-deb)"
WORK="$BUILD_ROOT/source"
OUTPUT="${QTIDM_DEB_OUTPUT_DIR:-$ROOT/dist}"
REQUESTED_ARCH=""
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

usage() {
    cat <<'EOF'
Usage: sh packaging/build-deb.sh [--arch amd64|arm64]

  --arch    Target architecture (amd64 or arm64). Defaults to host architecture.

  QTIDM_DEB_OUTPUT_DIR   Directory for the finished Debian packages (default: dist/).
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --arch)
            REQUESTED_ARCH="${2:-}"
            shift 2
            ;;
        --arch=*)
            REQUESTED_ARCH="${1#--arch=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

HOST_ARCH="$(packaging_host_arch || true)"
if [ -n "$REQUESTED_ARCH" ]; then
    if ! TARGET_ARCH="$(packaging_normalize_arch "$REQUESTED_ARCH")"; then
        echo "--arch must be amd64 or arm64, not '$REQUESTED_ARCH'." >&2
        exit 2
    fi
else
    TARGET_ARCH="$HOST_ARCH"
fi

mkdir -p "$WORK" "$OUTPUT"
assert_not_home_workspace "$BUILD_ROOT"

if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    DELETED_FILES="$BUILD_ROOT/deleted-files"
    git -C "$ROOT" ls-files --deleted > "$DELETED_FILES"
    git -C "$ROOT" ls-files -z --cached --others --exclude-standard \
        | tar -C "$ROOT" --exclude-from="$DELETED_FILES" \
            --null --files-from=- -cf - \
        | tar -C "$WORK" -xf -
else
    tar --exclude=build-deb-src --exclude='.build-deb.*' \
        --exclude=build --exclude='build-*' --exclude=dist \
        --exclude=.flatpak-builder --exclude=.git \
        --exclude=qtidm-chrome.pem --exclude='*.AppImage' \
        --exclude='*.buildinfo' --exclude='*.changes' \
        --exclude='*.deb' --exclude='*.ddeb' --exclude='*.flatpak' \
        -C "$ROOT" -cf - . | tar -C "$WORK" -xf -
fi

for package in qtidm-chrome.crx qtidm-firefox.xpi; do
    if [ -f "$ROOT/browser/packages/$package" ]; then
        mkdir -p "$WORK/browser/packages"
        cp "$ROOT/browser/packages/$package" "$WORK/browser/packages/$package"
    fi
done

rm -rf "$WORK/debian"
cp -r "$ROOT/packaging/debian" "$WORK/debian"
VERSION="$(sed -n 's/^project(qtIDM VERSION \([^ ]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
cat > "$WORK/debian/changelog" <<EOF
qtidm (${VERSION}-1) unstable; urgency=medium

  * Release ${VERSION}.

 -- qtIDM Maintainers <maintainers@example.invalid>  $(date -R)
EOF
cd "$WORK"
BUILD_ARGS="-us -uc -b"
if [ -n "$REQUESTED_ARCH" ]; then
    DEB_TARGET="$(case "$TARGET_ARCH" in x86_64) echo amd64 ;; aarch64) echo arm64 ;; esac)"
    BUILD_ARGS="-a$DEB_TARGET $BUILD_ARGS"
fi
dpkg-buildpackage $BUILD_ARGS
for artifact in "$BUILD_ROOT"/qtidm_*.deb "$BUILD_ROOT"/qtidm_*.ddeb "$BUILD_ROOT"/qtidm_*.changes "$BUILD_ROOT"/qtidm_*.buildinfo; do
    if [ -f "$artifact" ]; then
        cp "$artifact" "$OUTPUT/"
    fi
done
