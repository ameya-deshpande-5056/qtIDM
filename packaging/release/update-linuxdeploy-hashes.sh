#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKFLOW_FILE="$ROOT_DIR/.github/workflows/build-release.yml"
README_FILE="$ROOT_DIR/README.md"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "Fetching continuous linuxdeploy and linuxdeploy-plugin-qt AppImages..."
curl -sL --retry 3 -o "$TMPDIR/linuxdeploy.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
curl -sL --retry 3 -o "$TMPDIR/linuxdeploy-plugin-qt.AppImage" \
  https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

NEW_LINUXDEPLOY_SHA=$(sha256sum "$TMPDIR/linuxdeploy.AppImage" | awk '{print $1}')
NEW_PLUGIN_SHA=$(sha256sum "$TMPDIR/linuxdeploy-plugin-qt.AppImage" | awk '{print $1}')

echo "Fetched hashes:"
echo "  linuxdeploy:           $NEW_LINUXDEPLOY_SHA"
echo "  linuxdeploy-plugin-qt: $NEW_PLUGIN_SHA"

OLD_LINUXDEPLOY_SHA=$(grep -oP 'LINUXDEPLOY_SHA256:\s*\K[0-9a-f]{64}' "$WORKFLOW_FILE" | head -n 1 || true)
OLD_PLUGIN_SHA=$(grep -oP 'LINUXDEPLOY_PLUGIN_QT_SHA256:\s*\K[0-9a-f]{64}' "$WORKFLOW_FILE" | head -n 1 || true)

echo "Current workflow hashes:"
echo "  linuxdeploy:           $OLD_LINUXDEPLOY_SHA"
echo "  linuxdeploy-plugin-qt: $OLD_PLUGIN_SHA"

if [ "$NEW_LINUXDEPLOY_SHA" = "$OLD_LINUXDEPLOY_SHA" ] && [ "$NEW_PLUGIN_SHA" = "$OLD_PLUGIN_SHA" ]; then
  echo "Hashes are already up to date."
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "changed=false" >> "$GITHUB_OUTPUT"
  fi
  exit 0
fi

echo "Updating $WORKFLOW_FILE..."
if [ -n "$OLD_LINUXDEPLOY_SHA" ]; then
  sed -i "s/$OLD_LINUXDEPLOY_SHA/$NEW_LINUXDEPLOY_SHA/g" "$WORKFLOW_FILE"
fi
if [ -n "$OLD_PLUGIN_SHA" ]; then
  sed -i "s/$OLD_PLUGIN_SHA/$NEW_PLUGIN_SHA/g" "$WORKFLOW_FILE"
fi

if [ -f "$README_FILE" ]; then
  echo "Updating $README_FILE..."
  if [ -n "$OLD_LINUXDEPLOY_SHA" ]; then
    sed -i "s/$OLD_LINUXDEPLOY_SHA/$NEW_LINUXDEPLOY_SHA/g" "$README_FILE"
  fi
  if [ -n "$OLD_PLUGIN_SHA" ]; then
    sed -i "s/$OLD_PLUGIN_SHA/$NEW_PLUGIN_SHA/g" "$README_FILE"
  fi
fi

if [ -n "${GITHUB_OUTPUT:-}" ]; then
  echo "changed=true" >> "$GITHUB_OUTPUT"
  echo "linuxdeploy_sha=$NEW_LINUXDEPLOY_SHA" >> "$GITHUB_OUTPUT"
  echo "plugin_sha=$NEW_PLUGIN_SHA" >> "$GITHUB_OUTPUT"
fi

echo "Successfully updated hashes."

