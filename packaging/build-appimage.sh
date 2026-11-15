#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-appimage)"
BUILD="$BUILD_ROOT/build"
APPDIR="$BUILD_ROOT/AppDir"
GENERATED="$BUILD_ROOT/output"
OUTPUT="${QTIDM_APPIMAGE_OUTPUT_DIR:-$ROOT/dist}"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
REQUESTED_ARCH=""
TOOLCHAIN=""
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

usage() {
    cat <<'EOF'
Usage: sh packaging/build-appimage.sh [--arch amd64|arm64] [--toolchain FILE]

  --arch        Target architecture (amd64/x86_64 or arm64/aarch64). Defaults to host architecture.
  --toolchain   CMake toolchain file for cross builds.

  QTIDM_APPIMAGE_OUTPUT_DIR   Directory for the finished AppImage (default: dist/).
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
    echo "Building $TARGET_ARCH AppImage on ${HOST_ARCH:-this machine} needs --toolchain FILE," >&2
    echo "or run this script on a machine of that architecture." >&2
    exit 1
fi

ARTIFACT="$OUTPUT/qtIDM-$VERSION-$TARGET_ARCH.AppImage"
assert_not_home_workspace "$BUILD_ROOT"
mkdir -p "$GENERATED" "$OUTPUT"

CMAKE_ARGS="-S $ROOT -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DQTIDM_BUILD_TESTS=OFF -DQTIDM_CHROME_EXTENSION_ID=${QTIDM_CHROME_EXTENSION_ID:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}"
if [ -n "$TOOLCHAIN" ]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN"
fi
cmake $CMAKE_ARGS
cmake --build "$BUILD"
DESTDIR="$APPDIR" cmake --install "$BUILD"

FFMPEG="$(command -v ffmpeg || true)"
if [ -z "$FFMPEG" ]; then
    echo "ffmpeg is required to build adaptive-media support into the AppImage." >&2
    exit 1
fi
cp -L "$FFMPEG" "$APPDIR/usr/bin/ffmpeg"

SEVENZIP="$(command -v 7zz || command -v 7z || true)"
if [ -n "$SEVENZIP" ]; then
    cp -L "$SEVENZIP" "$APPDIR/usr/bin/7zz"
    ln -sf 7zz "$APPDIR/usr/bin/7z"
fi

SECRET_TOOL="$(command -v secret-tool || true)"
if [ -n "$SECRET_TOOL" ]; then
    cp -L "$SECRET_TOOL" "$APPDIR/usr/bin/secret-tool"
fi

YTDLP="$(command -v yt-dlp || true)"
if [ -n "$YTDLP" ]; then
    cp -L "$YTDLP" "$APPDIR/usr/bin/yt-dlp"
fi

DENO="$(command -v deno || true)"
if [ -n "$DENO" ]; then
    cp -L "$DENO" "$APPDIR/usr/bin/deno"
fi

if ! command -v linuxdeploy >/dev/null 2>&1; then
    echo "linuxdeploy is required to build the AppImage." >&2
    exit 1
fi

cp "$ROOT/packaging/appimage/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"

DESKTOP_FILE="$ROOT/assets/io.qtidm.Qtidm.desktop"
ICON_FILE="$ROOT/assets/icons/hicolor/256x256/apps/io.qtidm.Qtidm.png"

cp "$DESKTOP_FILE" "$APPDIR/io.qtidm.Qtidm.desktop"
cp "$ICON_FILE" "$APPDIR/io.qtidm.Qtidm.png"
ln -sf io.qtidm.Qtidm.png "$APPDIR/.DirIcon"

(
    cd "$GENERATED"
    linuxdeploy --appdir "$APPDIR" \
        --desktop-file "$DESKTOP_FILE" \
        --icon-file "$ICON_FILE" \
        --plugin qt \
        --output appimage
)

GENERATED_APPIMAGE="$GENERATED/qtIDM-$TARGET_ARCH.AppImage"
if [ ! -f "$GENERATED_APPIMAGE" ]; then
    echo "linuxdeploy did not produce the expected qtIDM-$TARGET_ARCH.AppImage artifact." >&2
    exit 1
fi
cp "$GENERATED_APPIMAGE" "$ARTIFACT"
chmod +x "$ARTIFACT"
echo "Created $ARTIFACT"
