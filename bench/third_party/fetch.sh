#!/usr/bin/env bash
# bench/third_party/fetch.sh — pinned fetch recipe (rule 10): downloads the
# versions.txt artifacts into bench/third_party/src/, verifies every sha256,
# and refuses to continue on ANY mismatch. Distro packages remain the
# default CI install path during calibration (their versions are recorded
# per-run in the methodology headers and cross-checked against
# versions.txt); this recipe is the vendoring-compatible path and the
# reproducibility anchor for the pins.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p src && cd src

fetch() {  # fetch <name> <url> <sha256>
    local name="$1" url="$2" want="$3"
    if [ -f "$name" ] && [ "$(sha256sum "$name" | cut -d' ' -f1)" = "$want" ]; then
        echo "cached+verified: $name"; return 0
    fi
    echo "fetching: $url"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$name.part" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$name.part" "$url"
    else
        python3 - "$url" "$name.part" <<'PY'
import shutil, sys, urllib.request
req = urllib.request.Request(sys.argv[1], headers={"User-Agent": "chronokv-fetch"})
with urllib.request.urlopen(req, timeout=300) as r, open(sys.argv[2], "wb") as f:
    shutil.copyfileobj(r, f)
PY
    fi
    local got; got="$(sha256sum "$name.part" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        rm -f "$name.part"
        echo "FATAL: $name sha256 mismatch: got $got want $want" >&2
        echo "       do NOT blind-accept — see versions.txt's re-record rule" >&2
        exit 1
    fi
    mv "$name.part" "$name"
    echo "verified: $name ($got)"
}

fetch sqlite-amalgamation-3450100.zip \
      https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip \
      5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4
fetch lmdb-0.9.31.tar.gz \
      https://github.com/LMDB/lmdb/archive/refs/tags/LMDB_0.9.31.tar.gz \
      dd70a8c67807b3b8532b3e987b0a4e998962ecc28643e1af5ec77696b081c9b0
fetch rocksdb-8.9.1.tar.gz \
      https://github.com/facebook/rocksdb/archive/refs/tags/v8.9.1.tar.gz \
      c22d2097e7aa75629612fd020499bdae0d3e321c7bc4361960c42aaf9cbd6dc1
echo "all pins verified under bench/third_party/src/ — per-engine build notes in versions.txt"
