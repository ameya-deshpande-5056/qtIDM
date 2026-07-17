#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/.."
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"

cmake -S "$ROOT" -B "$BUILD/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD/build"
DESTDIR="$APPDIR" cmake --install "$BUILD/build"

cp "$ROOT/packaging/appimage/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"

linuxdeploy --appdir "$APPDIR" --plugin qt --output appimage
