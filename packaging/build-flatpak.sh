#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-flatpak)"
REQUESTED_ARCH=""
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

usage() {
    cat <<'EOF'
Usage: sh packaging/build-flatpak.sh [--arch amd64|arm64]

  --arch    Target architecture (amd64/x86_64 or arm64/aarch64). Defaults to host architecture.
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

assert_not_home_workspace "$BUILD_ROOT"

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
    --arch="${TARGET_ARCH:-x86_64}" \
    --state-dir="$BUILD_ROOT/state" \
    "$BUILD_ROOT/build-flatpak" \
    "$ROOT/packaging/flatpak/io.qtidm.Qtidm.yml"
