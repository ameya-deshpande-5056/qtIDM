#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/qtidm-deb.XXXXXX")"
WORK="$BUILD_ROOT/source"
OUTPUT="$ROOT/dist"
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM
mkdir -p "$WORK" "$OUTPUT"
tar --exclude=build-deb-src --exclude='.build-deb.*' --exclude=build --exclude='build-*' --exclude=dist --exclude=.git -C "$ROOT" -cf - . | tar -C "$WORK" -xf -
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
