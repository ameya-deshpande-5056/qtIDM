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
ARTIFACT="$OUTPUT/qtIDM-$VERSION-x86_64.AppImage"
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

assert_not_home_workspace "$BUILD_ROOT"
mkdir -p "$GENERATED" "$OUTPUT"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DQTIDM_BUILD_TESTS=OFF -DQTIDM_CHROME_EXTENSION_ID="${QTIDM_CHROME_EXTENSION_ID:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}"
cmake --build "$BUILD"
DESTDIR="$APPDIR" cmake --install "$BUILD"

FFMPEG="$(command -v ffmpeg || true)"
if [ -z "$FFMPEG" ]; then
    echo "ffmpeg is required to build adaptive-media support into the AppImage." >&2
    exit 1
fi
cp -L "$FFMPEG" "$APPDIR/usr/bin/ffmpeg"

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

GENERATED_APPIMAGE="$GENERATED/qtIDM-x86_64.AppImage"
if [ ! -f "$GENERATED_APPIMAGE" ]; then
    echo "linuxdeploy did not produce the expected qtIDM-x86_64.AppImage artifact." >&2
    exit 1
fi
cp "$GENERATED_APPIMAGE" "$ARTIFACT"
chmod +x "$ARTIFACT"
echo "Created $ARTIFACT"
