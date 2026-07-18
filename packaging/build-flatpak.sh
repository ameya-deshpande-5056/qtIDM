#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-flatpak)"
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

assert_not_home_workspace "$BUILD_ROOT"

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
    --state-dir="$BUILD_ROOT/state" \
    "$BUILD_ROOT/build-flatpak" \
    "$ROOT/packaging/flatpak/io.github.qtidm.qtidm.yml"
