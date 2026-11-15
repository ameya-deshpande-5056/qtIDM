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
trap 'rm -rf "$BUILD_ROOT"' EXIT HUP INT TERM

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

rpmbuild -bb --target x86_64 \
    --define "_topdir $TOP" \
    "$TOP/SPECS/qtidm.spec"

find "$TOP/RPMS" -name '*.rpm' -exec cp {} "$OUTPUT/" \;
if ! ls "$OUTPUT"/qtidm-*.rpm >/dev/null 2>&1; then
    echo "rpmbuild did not produce an RPM artifact." >&2
    exit 1
fi
echo "Created $(ls "$OUTPUT"/qtidm-*.rpm | tail -1)"
