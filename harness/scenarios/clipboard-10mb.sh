#!/usr/bin/env bash
# proc: deskflow-core
# automation: full
# seat: hackintosh
# description: place a 10 MB PNG on the pasteboard each iteration (server clipboard copies + marshall per client)
#
# Writes an incompressible 10 MB PNG once, then alternates it with a tiny
# text clipboard so every iteration is a real pasteboard change for the
# server's clipboard sync. Sourced by harness/run-scenario.sh.

CLIP_PNG=""

scenario_setup() {
  CLIP_PNG="$(mktemp -t harness-clip).png"
  harness_python - "$CLIP_PNG" <<'PY' || return 1
import os, struct, sys, zlib
path = sys.argv[1]
# ~10 MB of random RGB rows, stored (zlib level 0) so the file stays ~10 MB
w, h = 1024, 3413
raw = b"".join(b"\x00" + os.urandom(w * 3) for _ in range(h))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
with open(path, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n")
    f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
    f.write(chunk(b"IDAT", zlib.compress(raw, 0)))
    f.write(chunk(b"IEND", b""))
PY
  harness_log "png: $CLIP_PNG ($(stat -f%z "$CLIP_PNG") bytes)"
}

scenario_iter() {
  local n="$1"
  osascript -e "set the clipboard to (read (POSIX file \"$CLIP_PNG\") as «class PNGf»)" || return 1
  harness_sleep 1.5
  printf 'harness clipboard-10mb iter %s\n' "$n" | pbcopy
  harness_sleep 1.5
  if [ $((n % 50)) = 0 ]; then harness_log "iter $n done"; fi
  return 0
}

scenario_teardown() {
  printf '' | pbcopy
  if [ -n "$CLIP_PNG" ]; then rm -f "$CLIP_PNG"; fi
  return 0
}
