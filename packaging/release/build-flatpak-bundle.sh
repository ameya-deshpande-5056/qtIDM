#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
BUILD_DIR="$ROOT/build-flatpak"
REPO_DIR="$ROOT/build-flatpak-repo"

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean --repo="$REPO_DIR" "$BUILD_DIR" "$ROOT/packaging/flatpak/io.github.qtidm.qtidm.yml"
flatpak build-bundle "$REPO_DIR" "$ROOT/dist/qtIDM-$VERSION.flatpak" io.github.qtidm.qtidm
