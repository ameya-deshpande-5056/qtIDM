#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-flatpak-release)"
BUILD_DIR="$BUILD_ROOT/build-flatpak"
REPO_DIR="$BUILD_ROOT/build-flatpak-repo"
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

assert_not_home_workspace "$BUILD_ROOT"

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
    --state-dir="$BUILD_ROOT/state" \
    --repo="$REPO_DIR" \
    "$BUILD_DIR" \
    "$ROOT/packaging/flatpak/io.qtidm.Qtidm.yml"
mkdir -p "$ROOT/dist"
flatpak build-bundle "$REPO_DIR" "$ROOT/dist/qtIDM-$VERSION.flatpak" io.qtidm.Qtidm
