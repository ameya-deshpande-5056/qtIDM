#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/.."
WORK="$ROOT/build-deb-src"
rm -rf "$WORK"
mkdir -p "$WORK"
tar --exclude=build-deb-src --exclude=build --exclude='build-*' --exclude=dist --exclude=.git -C "$ROOT" -cf - . | tar -C "$WORK" -xf -
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
