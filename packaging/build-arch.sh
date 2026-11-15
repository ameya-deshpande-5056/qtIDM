#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
BUILD_ROOT="$(packaging_workspace qtidm-arch)"
WORK="$BUILD_ROOT/pkg"
STAGE="$BUILD_ROOT/qtidm-$VERSION"
OUTPUT="${QTIDM_ARCH_OUTPUT_DIR:-$ROOT/dist}"
PKG_TAR_EXT="zst"
REQUESTED_ARCH=""
TOOLCHAIN=""
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

usage() {
    cat <<'EOF'
Usage: sh packaging/build-arch.sh [--pkg-ext zst|xz] [--arch amd64|arm64] [--toolchain FILE]

  --pkg-ext     Compression of the package: zst (default) or xz.
  --arch        Target architecture. Defaults to the architecture of this machine.
  --toolchain   CMake toolchain file, needed to cross-build for the other
                architecture. Without it, build on an arm64 machine instead.

  QTIDM_ARCH_OUTPUT_DIR   Directory for the finished package (default: dist/).
EOF
}

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
        --arch)
            REQUESTED_ARCH="${2:-}"
            shift 2
            ;;
        --arch=*)
            REQUESTED_ARCH="${1#--arch=}"
            shift
            ;;
        --toolchain)
            TOOLCHAIN="${2:-}"
            shift 2
            ;;
        --toolchain=*)
            TOOLCHAIN="${1#--toolchain=}"
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

case "$PKG_TAR_EXT" in
    zst|xz) ;;
    *)
        echo "--pkg-ext must be zst or xz, not '$PKG_TAR_EXT'." >&2
        exit 2
        ;;
esac

HOST_ARCH="$(packaging_host_arch || true)"
if [ -n "$REQUESTED_ARCH" ]; then
    if ! TARGET_ARCH="$(packaging_normalize_arch "$REQUESTED_ARCH")"; then
        echo "--arch must be amd64 or arm64, not '$REQUESTED_ARCH'." >&2
        exit 2
    fi
else
    TARGET_ARCH="$HOST_ARCH"
fi
if [ -z "$TARGET_ARCH" ]; then
    echo "Cannot detect this machine's architecture; pass --arch amd64 or --arch arm64." >&2
    exit 1
fi

if [ -n "$TOOLCHAIN" ] && [ ! -f "$TOOLCHAIN" ]; then
    echo "Toolchain file not found: $TOOLCHAIN" >&2
    exit 2
fi
if [ "$TARGET_ARCH" != "$HOST_ARCH" ] && [ -z "$TOOLCHAIN" ]; then
    echo "Building $TARGET_ARCH packages on ${HOST_ARCH:-this machine} needs --toolchain FILE," >&2
    echo "or run this script on a machine of that architecture, or in a matching container." >&2
    exit 1
fi

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
tar -C "$BUILD_ROOT" -czf "$WORK/qtidm-$VERSION.tar.gz" "qtidm-$VERSION"

# makepkg takes pkgver from the PKGBUILD and expects a matching source tarball,
# so tie both to the version CMakeLists.txt declares.
sed -e "s/^pkgver=.*/pkgver=$VERSION/" -e "s/^pkgrel=.*/pkgrel=1/" \
    "$ROOT/packaging/arch/PKGBUILD" > "$WORK/PKGBUILD"

(
    cd "$WORK"
    # CARCH selects the architecture makepkg stamps on the package, so it has to
    # follow the requested target rather than the machine running the build.
    PKGDEST="$OUTPUT" \
    PKGEXT=".pkg.tar.$PKG_TAR_EXT" \
    CARCH="$TARGET_ARCH" \
    QTIDM_CMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    makepkg --force --noconfirm
)

ARTIFACT="$(find "$OUTPUT" -type f -name "qtidm-$VERSION-*-$TARGET_ARCH.pkg.tar.$PKG_TAR_EXT" -print -quit)"
if [ -z "$ARTIFACT" ]; then
    echo "makepkg did not produce a $TARGET_ARCH package artifact." >&2
    exit 1
fi
echo "Created $ARTIFACT"
