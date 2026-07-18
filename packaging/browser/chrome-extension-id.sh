#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <chrome-extension-private-key.pem>" >&2
    exit 2
fi

KEY="$1"
if [ ! -f "$KEY" ]; then
    echo "Chrome extension private key does not exist: $KEY" >&2
    exit 1
fi

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
. "$ROOT/packaging/common.sh"

WORK="$(packaging_workspace qtidm-chrome-id)"
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
assert_not_home_workspace "$WORK"

openssl pkey -in "$KEY" -pubout -outform DER -out "$WORK/public-key.der" 2>/dev/null
set -- $(sha256sum "$WORK/public-key.der")
DIGEST="$1"

printf '%.32s\n' "$DIGEST" | tr '0123456789abcdef' 'abcdefghijklmnop'
