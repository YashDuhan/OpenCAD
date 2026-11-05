#!/bin/bash
# Runs every end-to-end scenario in scripts/ai/e2e/scenarios (those whose name starts with gemini_ only with REAL=gemini).
#   scripts/ai/e2e/run_all.sh [OUT_ROOT]
HERE=$(cd "$(dirname "$0")" && pwd)
OUT_ROOT=${1:-/tmp/opencad-e2e-results}
mkdir -p "$OUT_ROOT"
fails=0
for s in "$HERE"/scenarios/*/; do
    name=$(basename "$s")
    case "$name" in gemini_*) [ "${REAL:-}" = "gemini" ] || continue ;; *) [ "${REAL:-}" = "gemini" ] && continue ;; esac
    printf '%-28s ' "$name"
    "$HERE/run.sh" "$s" "$OUT_ROOT/$name" > "$OUT_ROOT/$name.txt" 2>&1
    code=$?
    head -4 "$OUT_ROOT/$name.txt"
    [ $code -eq 0 ] || fails=$((fails + 1))
done
echo "$fails scenario(s) failed; results in $OUT_ROOT"
exit $fails
