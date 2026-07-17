#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
VERSION="$(sed -n 's/^project(qtIDM VERSION \([^ ]*\).*/\1/p' "$ROOT/CMakeLists.txt")"

if [ -z "$VERSION" ]; then
  echo "Cannot determine version from CMakeLists.txt" >&2
  exit 1
fi

mkdir -p "$ROOT/dist"
cat > "$ROOT/dist/version-info.txt" <<EOF
name=qtIDM
version=$VERSION
tag=v$VERSION
commit=${GITHUB_SHA:-unknown}
source_date=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

printf '%s\n' "$VERSION"
