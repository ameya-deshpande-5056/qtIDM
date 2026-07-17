#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"

rm -rf "$APPDIR"

cmake -S "$ROOT" -B "$BUILD/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DQTIDM_BUILD_TESTS=OFF -DQTIDM_CHROME_EXTENSION_ID="${QTIDM_CHROME_EXTENSION_ID:-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa}"
cmake --build "$BUILD/build"
DESTDIR="$APPDIR" cmake --install "$BUILD/build"

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

linuxdeploy --appdir "$APPDIR" --plugin qt --output appimage
