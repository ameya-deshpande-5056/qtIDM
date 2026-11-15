#!/usr/bin/env sh
set -eu

packaging_workspace() {
    prefix="$1"
    mktemp -d "${TMPDIR:-/tmp}/${prefix}.XXXXXX"
}

# Normalizes an architecture name to the two spellings the RPM and Arch
# packages use: x86_64 for amd64 and aarch64 for arm64. Fails for anything else.
packaging_normalize_arch() {
    case "$1" in
        amd64|x86_64|x64) echo x86_64 ;;
        arm64|aarch64) echo aarch64 ;;
        *) return 1 ;;
    esac
}

packaging_host_arch() {
    packaging_normalize_arch "$(uname -m)" 2>/dev/null || true
}

assert_not_home_workspace() {
    workspace="$1"
    case "$workspace" in
        "$HOME"/*|"$HOME")
            echo "Refusing to use a workspace under HOME: $workspace" >&2
            exit 1
            ;;
    esac
}

# Copies the working tree into a destination directory, tracked and untracked
# files alike, without build output or packaging artifacts. Used by the RPM and
# Arch scripts, which both package from a source tarball.
stage_source_tree() {
    root="$1"
    destination="$2"
    if git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        deleted="$(mktemp)"
        git -C "$root" ls-files --deleted > "$deleted"
        git -C "$root" ls-files -z --cached --others --exclude-standard \
            | tar -C "$root" --exclude-from="$deleted" \
                --null --files-from=- -cf - \
            | tar -C "$destination" -xf -
        rm -f "$deleted"
    else
        tar --exclude=build --exclude='build-*' --exclude=build-deb-src \
            --exclude='.build-deb.*' --exclude=dist --exclude=.flatpak-builder \
            --exclude=.git --exclude=qtidm-chrome.pem --exclude='*.AppImage' \
            --exclude='*.buildinfo' --exclude='*.changes' --exclude='*.deb' \
            --exclude='*.ddeb' --exclude='*.flatpak' --exclude='*.rpm' \
            --exclude='*.pkg.tar.*' \
            -C "$root" -cf - . | tar -C "$destination" -xf -
    fi
}
