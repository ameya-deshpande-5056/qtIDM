#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORKFLOW_FILE="$ROOT_DIR/.github/workflows/build-release.yml"
README_FILE="$ROOT_DIR/README.md"
FLATPAK_FILE="$ROOT_DIR/packaging/flatpak/io.qtidm.Qtidm.yml"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

CHANGED=false

echo "Checking Flatpak dependencies (libsecret, 7zip, yt-dlp)..."
if [ -f "$FLATPAK_FILE" ]; then
  python3 - <<'EOF' "$FLATPAK_FILE"
import sys, json, re, hashlib, urllib.request

flatpak_file = sys.argv[1]
with open(flatpak_file, 'r', encoding='utf-8') as f:
    content = f.read()

changed = False

# 1. Update yt-dlp
try:
    req = urllib.request.Request('https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest', headers={'User-Agent': 'qtIDM'})
    with urllib.request.urlopen(req, timeout=15) as resp:
        tag = json.loads(resp.read().decode())['tag_name']

    sums_url = f'https://github.com/yt-dlp/yt-dlp/releases/download/{tag}/SHA2-256SUMS'
    req_sums = urllib.request.Request(sums_url, headers={'User-Agent': 'qtIDM'})
    with urllib.request.urlopen(req_sums, timeout=15) as resp:
        sums_text = resp.read().decode()

    yt_sha = None
    for line in sums_text.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[1] == 'yt-dlp_linux':
            yt_sha = parts[0]
            break

    lic_url = f'https://raw.githubusercontent.com/yt-dlp/yt-dlp/{tag}/LICENSE'
    req_lic = urllib.request.Request(lic_url, headers={'User-Agent': 'qtIDM'})
    with urllib.request.urlopen(req_lic, timeout=15) as resp:
        lic_sha = hashlib.sha256(resp.read()).hexdigest()

    if yt_sha and lic_sha:
        pattern_yt = r'(url:\s*https://github\.com/yt-dlp/yt-dlp/releases/download/)[^/]+(/yt-dlp_linux\s*\n\s*sha256:\s*)[0-9a-f]{64}'
        new_content, n1 = re.subn(pattern_yt, rf'\g<1>{tag}\g<2>{yt_sha}', content)
        pattern_lic = r'(url:\s*https://raw\.githubusercontent\.com/yt-dlp/yt-dlp/)[^/]+(/LICENSE\s*\n\s*sha256:\s*)[0-9a-f]{64}'
        new_content, n2 = re.subn(pattern_lic, rf'\g<1>{tag}\g<2>{lic_sha}', new_content)
        if n1 > 0 or n2 > 0:
            if new_content != content:
                content = new_content
                changed = True
                print(f"Updated Flatpak yt-dlp to {tag}")
except Exception as e:
    print(f"Warning: failed to check yt-dlp update: {e}", file=sys.stderr)

# 2. Update 7zip
try:
    req = urllib.request.Request('https://api.github.com/repos/ip7z/7zip/releases/latest', headers={'User-Agent': 'qtIDM'})
    with urllib.request.urlopen(req, timeout=15) as resp:
        tag = json.loads(resp.read().decode())['tag_name']

    tag_nodot = tag.replace('.', '')
    tar_url = f'https://github.com/ip7z/7zip/releases/download/{tag}/7z{tag_nodot}-linux-x64.tar.xz'
    req_tar = urllib.request.Request(tar_url, headers={'User-Agent': 'qtIDM'})
    with urllib.request.urlopen(req_tar, timeout=20) as resp:
        seven_sha = hashlib.sha256(resp.read()).hexdigest()

    pattern_7z = r'(url:\s*https://github\.com/ip7z/7zip/releases/download/)[^/]+(/7z)[0-9]+(-linux-x64\.tar\.xz\s*\n\s*sha256:\s*)[0-9a-f]{64}'
    new_content, n = re.subn(pattern_7z, rf'\g<1>{tag}\g<2>{tag_nodot}\g<3>{seven_sha}', content)
    if n > 0 and new_content != content:
        content = new_content
        changed = True
        print(f"Updated Flatpak 7zip to {tag}")
except Exception as e:
    print(f"Warning: failed to check 7zip update: {e}", file=sys.stderr)

# 3. Update libsecret
try:
    req = urllib.request.Request('https://download.gnome.org/sources/libsecret/0.21/', headers={'User-Agent': 'qtIDM'})
    with urllib.request.urlopen(req, timeout=15) as resp:
        html = resp.read().decode()

    versions = re.findall(r'libsecret-(0\.21\.[0-9]+)\.tar\.xz', html)
    if versions:
        latest_ver = sorted(set(versions), key=lambda v: [int(x) for x in v.split('.')])[-1]
        sha_url = f'https://download.gnome.org/sources/libsecret/0.21/libsecret-{latest_ver}.sha256sum'
        req_sha = urllib.request.Request(sha_url, headers={'User-Agent': 'qtIDM'})
        with urllib.request.urlopen(req_sha, timeout=15) as resp:
            sha_text = resp.read().decode()
        libsec_sha = None
        for line in sha_text.splitlines():
            parts = line.strip().split()
            if len(parts) >= 2 and parts[1] == f'libsecret-{latest_ver}.tar.xz':
                libsec_sha = parts[0]
                break
        if libsec_sha:
            pattern_sec = r'(url:\s*https://download\.gnome\.org/sources/libsecret/0\.21/libsecret-)[^/]+(\.tar\.xz\s*\n\s*sha256:\s*)[0-9a-f]{64}'
            new_content, n = re.subn(pattern_sec, rf'\g<1>{latest_ver}\g<2>{libsec_sha}', content)
            if n > 0 and new_content != content:
                content = new_content
                changed = True
                print(f"Updated Flatpak libsecret to {latest_ver}")
except Exception as e:
    print(f"Warning: failed to check libsecret update: {e}", file=sys.stderr)

if changed:
    with open(flatpak_file, 'w', encoding='utf-8') as f:
        f.write(content)
    with open(sys.argv[1] + '.changed', 'w') as f:
        f.write('true')
EOF
  if [ -f "$FLATPAK_FILE.changed" ]; then
    CHANGED=true
    rm -f "$FLATPAK_FILE.changed"
  fi
fi

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

if [ "$NEW_LINUXDEPLOY_SHA" != "$OLD_LINUXDEPLOY_SHA" ] || [ "$NEW_PLUGIN_SHA" != "$OLD_PLUGIN_SHA" ]; then
  CHANGED=true
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
fi

if [ "$CHANGED" = "true" ]; then
  echo "Successfully updated dependencies."
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "changed=true" >> "$GITHUB_OUTPUT"
    echo "linuxdeploy_sha=$NEW_LINUXDEPLOY_SHA" >> "$GITHUB_OUTPUT"
    echo "plugin_sha=$NEW_PLUGIN_SHA" >> "$GITHUB_OUTPUT"
  fi
else
  echo "Dependencies and hashes are already up to date."
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "changed=false" >> "$GITHUB_OUTPUT"
  fi
fi

