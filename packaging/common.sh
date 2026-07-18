#!/usr/bin/env sh
set -eu

packaging_workspace() {
    prefix="$1"
    mktemp -d "${TMPDIR:-/tmp}/${prefix}.XXXXXX"
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
