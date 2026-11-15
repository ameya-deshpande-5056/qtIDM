#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
. "$ROOT/packaging/common.sh"
BUILD_ROOT="$(packaging_workspace qtidm-rpm)"
TOP="$BUILD_ROOT/rpmbuild"
STAGE="$BUILD_ROOT/qtidm"
OUTPUT="${QTIDM_RPM_OUTPUT_DIR:-$ROOT/dist}"
VERSION="$(sh "$ROOT/packaging/release/version-info.sh")"
REQUESTED_ARCH=""
TOOLCHAIN=""
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

usage() {
    cat <<'EOF'
Usage: sh packaging/build-rpm.sh [--arch amd64|arm64] [--toolchain FILE]

  --arch        Target architecture. Defaults to the architecture of this machine.
  --toolchain   CMake toolchain file, needed to cross-build for the other
                architecture. Without it, build on a machine/an arm64 container of
                the target architecture instead.

  QTIDM_RPM_OUTPUT_DIR   Directory for the finished RPM (default: dist/).
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
        --toolchain)
            TOOLCHAIN="${2:-}"
            shift 2
            ;;
        --toolchain=*)
            TOOLCHAIN="${1#--toolchain=}"
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

if [ -n "$TOOLCHAIN" ] && [ ! -f "$TOOLCHAIN" ]; then
    echo "Toolchain file not found: $TOOLCHAIN" >&2
    exit 2
fi
if [ "$TARGET_ARCH" != "$HOST_ARCH" ] && [ -z "$TOOLCHAIN" ]; then
    echo "Building $TARGET_ARCH packages on ${HOST_ARCH:-this machine} needs --toolchain FILE," >&2
    echo "or run this script on a machine of that architecture, or in a matching container." >&2
    exit 1
fi

assert_not_home_workspace "$BUILD_ROOT"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild is required to build the RPM (install rpm-build on Fedora)." >&2
    exit 1
fi

mkdir -p "$TOP/SOURCES" "$TOP/SPECS" "$TOP/BUILD" "$TOP/RPMS" "$TOP/SRPMS" "$OUTPUT"
mkdir -p "$STAGE"
stage_source_tree "$ROOT" "$STAGE"
tar -C "$BUILD_ROOT" -czf "$TOP/SOURCES/qtidm-$VERSION.tar.gz" qtidm

# The spec's Version is authoritative for rpmbuild, so keep it tied to the
# project version instead of relying on a manual edit.
sed "s/^Version:.*/Version:        $VERSION/" \
    "$ROOT/packaging/rpm/qtidm.spec" > "$TOP/SPECS/qtidm.spec"

# rpmbuild has no argument for a toolchain file, so hand it to the spec as a
# macro; the spec passes it to CMake only when it is defined.
set -- --define "_topdir $TOP"
if [ -n "$TOOLCHAIN" ]; then
    set -- "$@" --define "qtidm_toolchain $TOOLCHAIN"
fi

rpmbuild -bb --target "$TARGET_ARCH" "$@" "$TOP/SPECS/qtidm.spec"

ARTIFACT="$(find "$TOP/RPMS" -type f -name "qtidm-$VERSION-*.$TARGET_ARCH.rpm" -print -quit)"
if [ -z "$ARTIFACT" ]; then
    echo "rpmbuild did not produce a $TARGET_ARCH RPM artifact." >&2
    exit 1
fi
cp "$ARTIFACT" "$OUTPUT/"
echo "Created $OUTPUT/$(basename "$ARTIFACT")"
