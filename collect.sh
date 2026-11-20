#!/bin/bash
# Run ON THE WORKING BOX once: pulls your latest sources from /root/drmprobe into this repo.
# The proprietary blob.so is NOT copied (it must not go into git); keep your own copy.
set -e
W=${W:-/root/drmprobe}; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/src" "$HERE/config"
for f in mali_shim.c gbm_compat.c fakedrm.c fakedrm_atomic.c fakeudev.c; do
  if [ -s "$W/$f" ]; then cp "$W/$f" "$HERE/src/$f"; echo "copied $f"; else echo "NOT FOUND: $W/$f"; fi
done
[ -s "$W/weston.ini" ] && cp "$W/weston.ini" "$HERE/config/weston.ini" && echo "copied weston.ini"
sha256sum "$W/lib/blob.so" 2>/dev/null | sed 's#/.*/##' > "$HERE/BLOB.sha256" || true
cat <<MSG

Next:
  cd $HERE && git init && git add -A && git commit -m "Mali-G31 Wayland/EGL stack"
  git remote add origin git@github.com:<you>/<repo>.git && git push -u origin main
Keep a copy of $W/lib/blob.so elsewhere (BLOB.sha256 records which one it is).
MSG
