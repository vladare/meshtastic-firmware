#!/usr/bin/env bash
# Fail if Board Finder protocol_version is not frozen at 1 in both trees.
set -euo pipefail

ROOT_FW="${1:-.}"
ROOT_AND="${2:-../Board Finder}"

fail() { echo "FAIL: $*" >&2; exit 1; }

grep -q 'PROTOCOL_VERSION = 1' "$ROOT_FW/src/modules/BoardFinder/boardfinder_protocol.h" \
  || fail "firmware PROTOCOL_VERSION != 1"
grep -Eq 'PROTOCOL_VERSION\s*=\s*[23]' "$ROOT_FW/src/modules/BoardFinder/boardfinder_protocol.h" \
  && fail "firmware still references PROTOCOL_VERSION 2/3" || true
grep -q 'PROTOCOL_VERSION: Int = 1' "$ROOT_AND/app/src/main/java/org/boardfinder/app/meshtastic/BoardFinderMessageCodec.kt" \
  || fail "Android PROTOCOL_VERSION != 1"
grep -Eq 'PROTOCOL_VERSION(: Int)? = [23]' "$ROOT_AND/app/src/main/java/org/boardfinder/app/meshtastic/BoardFinderMessageCodec.kt" \
  && fail "Android still references PROTOCOL_VERSION 2/3" || true

# Canonical proto must declare frozen version in comments / field 1 semantics.
grep -q 'protocol_version = 1' "$ROOT_FW/protocol/boardfinder.proto" \
  || fail "firmware protocol/boardfinder.proto missing freeze note"
grep -q 'protocol_version = 1' "$ROOT_AND/protocol/boardfinder.proto" \
  || fail "Android protocol/boardfinder.proto missing freeze note"

# Schemas must match.
diff -q "$ROOT_FW/protocol/boardfinder.proto" "$ROOT_AND/protocol/boardfinder.proto" \
  || fail "firmware and Android boardfinder.proto diverge"

echo "OK: Board Finder protocol freeze verified (version 1)"
