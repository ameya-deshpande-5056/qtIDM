#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SOURCE="$ROOT/assets/io.qtidm.Qtidm.svg"

command -v ffmpeg >/dev/null 2>&1 || {
    echo "ffmpeg with SVG input support is required to generate qtIDM icons." >&2
    exit 1
}

for size in 16 32 48 64 128 256 512; do
    destination="$ROOT/assets/icons/hicolor/${size}x${size}/apps"
    mkdir -p "$destination"
    ffmpeg -hide_banner -loglevel error -y -i "$SOURCE" \
        -vf "scale=${size}:${size}" -frames:v 1 "$destination/io.qtidm.Qtidm.png"
done

for browser in chrome firefox; do
    destination="$ROOT/browser/$browser/icons"
    mkdir -p "$destination"
    for size in 16 32 48 128; do
        ffmpeg -hide_banner -loglevel error -y -i "$SOURCE" \
            -vf "scale=${size}:${size}" -frames:v 1 "$destination/icon-${size}.png"
    done
done
