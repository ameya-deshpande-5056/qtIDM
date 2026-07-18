#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-deb)"
WORK="$BUILD_ROOT/source"
OUTPUT="$ROOT/dist"
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM
mkdir -p "$WORK" "$OUTPUT"
assert_not_home_workspace "$BUILD_ROOT"

if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    DELETED_FILES="$BUILD_ROOT/deleted-files"
    git -C "$ROOT" ls-files --deleted > "$DELETED_FILES"
    git -C "$ROOT" ls-files -z --cached --others --exclude-standard \
        | tar -C "$ROOT" --exclude-from="$DELETED_FILES" \
            --null --files-from=- -cf - \
        | tar -C "$WORK" -xf -
else
    tar --exclude=build-deb-src --exclude='.build-deb.*' \
        --exclude=build --exclude='build-*' --exclude=dist \
        --exclude=.flatpak-builder --exclude=.git \
        --exclude=qtidm-chrome.pem --exclude='*.AppImage' \
        --exclude='*.buildinfo' --exclude='*.changes' \
        --exclude='*.deb' --exclude='*.ddeb' --exclude='*.flatpak' \
        -C "$ROOT" -cf - . | tar -C "$WORK" -xf -
fi

for package in qtidm-chrome.crx qtidm-firefox.xpi; do
    if [ -f "$ROOT/browser/packages/$package" ]; then
        mkdir -p "$WORK/browser/packages"
        cp "$ROOT/browser/packages/$package" "$WORK/browser/packages/$package"
    fi
done

rm -rf "$WORK/debian"
cp -r "$ROOT/packaging/debian" "$WORK/debian"
VERSION="$(sed -n 's/^project(qtIDM VERSION \([^ ]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
cat > "$WORK/debian/changelog" <<EOF
qtidm (${VERSION}-1) unstable; urgency=medium

  * Release ${VERSION}.

 -- qtIDM Maintainers <maintainers@example.invalid>  $(date -R)
EOF
cd "$WORK"
dpkg-buildpackage -us -uc -b
for artifact in "$BUILD_ROOT"/qtidm_*.deb "$BUILD_ROOT"/qtidm_*.ddeb "$BUILD_ROOT"/qtidm_*.changes "$BUILD_ROOT"/qtidm_*.buildinfo; do
    if [ -f "$artifact" ]; then
        cp "$artifact" "$OUTPUT/"
    fi
done
