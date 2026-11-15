#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-flatpak-release)"
BUILD_DIR="$BUILD_ROOT/build-flatpak"
REPO_DIR="$BUILD_ROOT/build-flatpak-repo"
OUTPUT="${QTIDM_FLATPAK_OUTPUT_DIR:-$ROOT/dist}"
REQUESTED_ARCH=""
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

usage() {
    cat <<'EOF'
Usage: sh packaging/release/build-flatpak-bundle.sh [--arch amd64|arm64]

  --arch    Target architecture (amd64/x86_64 or arm64/aarch64). Defaults to host architecture.

  QTIDM_FLATPAK_OUTPUT_DIR   Directory for the finished Flatpak bundle (default: dist/).
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --arch)
            REQUESTED_ARCH="${2:-}"
            shift 2
            ;;
        --arch=*)
            REQUESTED_ARCH="${1#--arch=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

HOST_ARCH="$(packaging_host_arch || true)"
if [ -n "$REQUESTED_ARCH" ]; then
    if ! TARGET_ARCH="$(packaging_normalize_arch "$REQUESTED_ARCH")"; then
        echo "--arch must be amd64 or arm64, not '$REQUESTED_ARCH'." >&2
        exit 2
    fi
else
    TARGET_ARCH="$HOST_ARCH"
fi
if [ -z "$TARGET_ARCH" ]; then
    echo "Cannot detect this machine's architecture; pass --arch amd64 or --arch arm64." >&2
    exit 1
fi

assert_not_home_workspace "$BUILD_ROOT"

MANIFEST="$BUILD_ROOT/io.qtidm.Qtidm.yml"
sed "s|path: \.\./\.\.*|path: $ROOT|" "$ROOT/packaging/flatpak/io.qtidm.Qtidm.yml" > "$MANIFEST"

if command -v curl >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    DENO_TAG="$(curl -sI "https://github.com/denoland/deno/releases/latest/download/deno-$TARGET_ARCH-unknown-linux-gnu.zip" | grep -i '^location:' | sed -E 's|.*/download/([^/]+)/.*|\1|' | tr -d '\r\n')"
    if [ -n "$DENO_TAG" ]; then
        LATEST_ZIP="$BUILD_ROOT/deno-latest-$TARGET_ARCH.zip"
        if curl -fsSL -o "$LATEST_ZIP" "https://github.com/denoland/deno/releases/latest/download/deno-$TARGET_ARCH-unknown-linux-gnu.zip"; then
            LATEST_SHA="$(sha256sum "$LATEST_ZIP" | awk '{print $1}')"
            python3 -c "
import re, sys
manifest_path, arch, tag, sha = sys.argv[1:5]
with open(manifest_path, 'r', encoding='utf-8') as f:
    content = f.read()
pattern = rf'(url:\s*https://github\.com/denoland/deno/releases/download/)[^/]+(/deno-{arch}-unknown-linux-gnu\.zip\s*\n\s*sha256:\s*)[0-9a-f]{{64}}'
updated = re.sub(pattern, rf'\g<1>{tag}\g<2>{sha}', content)
with open(manifest_path, 'w', encoding='utf-8') as f:
    f.write(updated)
" "$MANIFEST" "$TARGET_ARCH" "$DENO_TAG" "$LATEST_SHA"
            rm -f "$LATEST_ZIP"
        fi
    fi
fi

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
    --arch="$TARGET_ARCH" \
    --state-dir="$BUILD_ROOT/state" \
    --repo="$REPO_DIR" \
    "$BUILD_DIR" \
    "$MANIFEST"
mkdir -p "$OUTPUT"
ARTIFACT="$OUTPUT/qtIDM-$VERSION-$TARGET_ARCH.flatpak"
flatpak build-bundle --arch="$TARGET_ARCH" "$REPO_DIR" "$ARTIFACT" io.qtidm.Qtidm
cp -f "$ARTIFACT" "$OUTPUT/qtIDM-$VERSION.flatpak"
echo "Created $ARTIFACT"
