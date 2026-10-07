#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! $1 =~ ^(3|5)$ ]]; then
    printf 'Usage: bash %s 3\n       bash %s 5\n' "$0" "$0" >&2
    exit 1
fi
n=$1
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
compiler=${CC:-cc}
if [[ ! -r /proc/self/stat ]]; then
    printf '%s\n' 'pstree requires Linux /proc. Run this script on a Linux system with /proc available.' >&2
    exit 1
fi
for dependency in "$compiler" pstree ps setsid; do
    if ! command -v "$dependency" >/dev/null 2>&1; then
        printf 'Required command not found: %s\n' "$dependency" >&2
        exit 1
    fi
done
"$compiler" -std=c11 -Wall -Wextra -Wpedantic -O2 \
    "$root/ex3.c" -o "$root/ex3"

program_pid=''
cleanup() {
    if [[ -n $program_pid ]]; then
        kill -TERM -- "-$program_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

setsid "$root/ex3" "$n" &
program_pid=$!
printf 'Started ex3 n=%d in background; root PID=%d\n' "$n" "$program_pid"

sleep 0.5
for ((round = 1; round <= n; ++round)); do
    count=$(ps -o pid= --sid "$program_pid" | wc -l)
    count=${count//[[:space:]]/}
    printf '\nSnapshot %d (~%d.5 s): total processes=%s\n' \
        "$round" "$((5 * (round - 1)))" "$count"
    pstree -p -c -l -U "$program_pid"
    if ((round < n)); then
        sleep 5
    fi
done

result=0
wait "$program_pid" || result=$?
program_pid=''
printf '\nExperiment finished; exit code=%d\n' "$result"
exit "$result"
