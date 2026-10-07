#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 0 ]]; then
    printf 'Usage: bash %s\n' "$0" >&2
    exit 1
fi

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
compiler=${CC:-cc}
"$compiler" -std=c11 -Wall -Wextra -Wpedantic -O2 \
    "$root/ex1.c" -o "$root/ex1"
exec "$root/ex1"
