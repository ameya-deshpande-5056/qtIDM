#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-arch)"
WORK="$BUILD_ROOT/pkg"
STAGE="$BUILD_ROOT/qtidm"
OUTPUT="${QTIDM_ARCH_OUTPUT_DIR:-$ROOT/dist}"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
PKG_TAR_EXT="zst"
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

while [ "$#" -gt 0 ]; do
    case "$1" in
        --pkg-ext)
            PKG_TAR_EXT="${2:-}"
            shift 2
            ;;
        --pkg-ext=*)
            PKG_TAR_EXT="${1#--pkg-ext=}"
            shift
            ;;
        -h|--help)
            echo "Usage: sh packaging/build-arch.sh [--pkg-ext zst|xz]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

case "$PKG_TAR_EXT" in
    zst|xz) ;;
    *)
        echo "--pkg-ext must be zst or xz, not '$PKG_TAR_EXT'." >&2
        exit 2
        ;;
esac

assert_not_home_workspace "$BUILD_ROOT"

if ! command -v makepkg >/dev/null 2>&1; then
    echo "makepkg is required to build the Arch package (install base-devel)." >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    echo "makepkg cannot run as root; build the Arch package as a normal user." >&2
    exit 1
fi

mkdir -p "$WORK" "$OUTPUT"
mkdir -p "$STAGE"
stage_source_tree "$ROOT" "$STAGE"
tar -C "$BUILD_ROOT" -czf "$WORK/qtidm-$VERSION.tar.gz" qtidm

# makepkg takes pkgver from the PKGBUILD and expects a matching source tarball,
# so tie both to the version CMakeLists.txt declares.
sed -e "s/^pkgver=.*/pkgver=$VERSION/" -e "s/^pkgrel=.*/pkgrel=1/" \
    "$ROOT/packaging/arch/PKGBUILD" > "$WORK/PKGBUILD"

(
    cd "$WORK"
    PKGDEST="$OUTPUT" \
    PKGEXT=".pkg.tar.$PKG_TAR_EXT" \
    makepkg --force --noconfirm
)

if ! ls "$OUTPUT"/qtidm-*.pkg.tar.* >/dev/null 2>&1; then
    echo "makepkg did not produce a package artifact." >&2
    exit 1
fi
echo "Created $(ls "$OUTPUT"/qtidm-*.pkg.tar.* | tail -1)"
