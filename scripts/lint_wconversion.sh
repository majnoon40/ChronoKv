#!/usr/bin/env bash
# -Wconversion ratchet for chronokv.hpp (v29 CI hardening).
#
# Compiles the header alone with -fsyntax-only -Wconversion (no codegen: a few
# seconds, none of the >1 GiB RSS of a real build) and compares the warning
# COUNT with ci/wconversion.baseline. The count may fall (fix a site, lower the
# baseline) but may never rise: a NEW implicit narrowing conversion fails CI.
#
# Why: the uint16_t page-offset narrowing class (audit CKV-001/002, and the
# update-path `uint16_t new_off = free_hi(p) - value.size()`) compiles silently
# under -Wall -Wextra; -Wconversion flags every such site. -Wsign-conversion and
# -Wfloat-conversion are excluded (unrelated, benign noise); integer-width
# narrowing is what is ratcheted.
#
# The count is compiler-version dependent. The baseline was recorded with
# g++ 13.3 (ubuntu-24.04's default). If the CI image's default g++ changes,
# regenerate with `scripts/lint_wconversion.sh --update` and review the diff.
#
# Env: CXX (default g++), BASELINE_FILE (default ci/wconversion.baseline).
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-g++}"
BASELINE_FILE="${BASELINE_FILE:-ci/wconversion.baseline}"
log="$(mktemp)"; trap 'rm -f "$log"' EXIT

printf '#include "chronokv.hpp"\n' | "$CXX" -std=c++20 -I. -fsyntax-only \
    -Wconversion -Wno-sign-conversion -Wno-float-conversion -x c++ - 2>"$log" || true

if grep -q 'error:' "$log"; then
    echo "::error::lint_wconversion: header failed to compile (not a warning issue)"
    grep 'error:' "$log" | head -20
    exit 2
fi

count=$(grep -c 'warning:' "$log" || true)

if [ "${1:-}" = "--update" ]; then
    echo "$count" > "$BASELINE_FILE"
    echo "baseline set to $count ($CXX)"
    exit 0
fi

base=$(head -n1 "$BASELINE_FILE" | tr -d ' \r\n')
echo "-Wconversion warnings: $count (baseline $base, $CXX)"

if [ "$count" -gt "$base" ]; then
    echo "::error::-Wconversion count rose from $base to $count: a new implicit narrowing conversion was added"
    echo "Warnings (compare against the known set; fix the new one, do NOT raise the baseline):"
    grep 'warning:' "$log" | sed -E 's#^[^:]*/##' | cut -c1-200
    exit 1
fi
if [ "$count" -lt "$base" ]; then
    echo "::notice::-Wconversion count fell to $count: lower ci/wconversion.baseline to lock it in (scripts/lint_wconversion.sh --update)"
fi
