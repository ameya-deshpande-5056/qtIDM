#!/usr/bin/env sh
set -eu

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR="$PROJECT_DIR/build-install"

case "$(uname -s)" in
    Linux)
        PREFIX=/usr/local
        MANIFEST="$PREFIX/share/qtidm/install_manifest.txt"
        BUILD_ARGS=
        INSTALL_ARGS=
        ;;
    MINGW*|MSYS*|CYGWIN*)
        PREFIX=$(cygpath -m "${LOCALAPPDATA:?LOCALAPPDATA is required}/Programs/qtIDM")
        MANIFEST="$PREFIX/install_manifest.txt"
        BUILD_ARGS="--config Release"
        INSTALL_ARGS="--config Release"
        ;;
    *)
        echo "Unsupported platform." >&2
        exit 1
        ;;
esac

if [ -f "$MANIFEST" ]; then
    sh "$PROJECT_DIR/uninstall.sh"
fi

rm -rf "$BUILD_DIR"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DQTIDM_BUILD_TESTS=OFF \
    -DQTIDM_INSTALL_NATIVE_MESSAGING_HOSTS=OFF \
    -DQTIDM_INSTALL_EXTERNAL_CHROME_EXTENSION=OFF
cmake --build "$BUILD_DIR" ${BUILD_ARGS:+$BUILD_ARGS}

case "$(uname -s)" in
    Linux)
        sudo cmake --install "$BUILD_DIR"
        sudo install -Dm644 "$BUILD_DIR/install_manifest.txt" "$MANIFEST"
        ;;
    *)
        cmake --install "$BUILD_DIR" ${INSTALL_ARGS:+$INSTALL_ARGS}
        cmake -E copy "$BUILD_DIR/install_manifest.txt" "$MANIFEST"
        ;;
esac
