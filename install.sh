#!/usr/bin/env sh
set -eu

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$PROJECT_DIR/build-install"
PREFIX=/usr/local
MANIFEST="$PREFIX/share/qtidm/install_manifest.txt"

if [ -f "$MANIFEST" ]; then
    "$PROJECT_DIR/uninstall.sh"
fi

rm -rf "$BUILD_DIR"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DQTIDM_BUILD_TESTS=OFF \
    -DQTIDM_INSTALL_NATIVE_MESSAGING_HOSTS=OFF \
    -DQTIDM_INSTALL_EXTERNAL_CHROME_EXTENSION=OFF
cmake --build "$BUILD_DIR"
sudo cmake --install "$BUILD_DIR"
sudo install -Dm644 "$BUILD_DIR/install_manifest.txt" "$MANIFEST"
