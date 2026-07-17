#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
VERSION="${1:-$(sh "$ROOT/packaging/release/version-info.sh")}"
mkdir -p "$ROOT/dist"

awk -v version="$VERSION" '
  $0 == "## " version { capture=1; next }
  capture && /^## / { exit }
  capture { print }
' "$ROOT/CHANGELOG.md" > "$ROOT/dist/CHANGELOG-$VERSION.md"

if [ ! -s "$ROOT/dist/CHANGELOG-$VERSION.md" ]; then
  cp "$ROOT/CHANGELOG.md" "$ROOT/dist/CHANGELOG-$VERSION.md"
fi
