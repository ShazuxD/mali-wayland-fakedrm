#!/bin/bash
# Install the Mali-G31 GPU userspace stack on a Debian arm64 rootfs (kernel 4.9, no real DRM).
# Uses the Rockchip libmali "bifrost-g31-g13p0-wayland-gbm" blob, which has its own Wayland+GBM EGL (no wrapper).
# Usage: sudo ./install.sh [--blob /path/libmali-bifrost-g31-g13p0-wayland-gbm.so] [--no-apt]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); M=/opt/mali-wl; BLOB=""; APT=1
NAME=libmali-bifrost-g31-g13p0-wayland-gbm.so
URL=https://github.com/jethome-iot/rockchip-libmali/raw/887a50b43b12ffbc39aa377f288799c05708a72f/lib/aarch64-linux-gnu/$NAME
while [ $# -gt 0 ]; do case $1 in --blob) BLOB=$2; shift 2;; --no-apt) APT=0; shift;; *) echo "unknown arg $1"; exit 1;; esac; done
for f in mali_shim.c gbm_compat.c fakedrm.c fakeudev.c; do [ -s "$HERE/src/$f" ] || { echo "missing src/$f"; exit 1; }; done
if [ $APT = 1 ]; then
  apt-get update
  apt-get install -y gcc libc6-dev libwayland-dev libdrm-dev libwayland-client0 weston firefox-esr mpv
fi
# --- blob: explicit path, a copy next to the repo, or the pinned download (the blob is proprietary and is NOT stored in this repo)
if [ -z "$BLOB" ]; then
  for c in "$HERE/blob/$NAME" "./$NAME"; do [ -s "$c" ] && BLOB=$c && break; done
fi
if [ -z "$BLOB" ]; then
  mkdir -p "$HERE/blob"; echo "downloading $NAME ..."
  if command -v wget >/dev/null; then wget -q -O "$HERE/blob/$NAME" "$URL"; else curl -fsSL -o "$HERE/blob/$NAME" "$URL"; fi
  BLOB=$HERE/blob/$NAME
fi
file -L "$BLOB" | grep -q 'ARM aarch64' || { echo "$BLOB is not an aarch64 ELF"; exit 1; }
WANT=$(cut -d' ' -f1 "$HERE/BLOB.sha256"); GOT=$(sha256sum "$BLOB" | cut -d' ' -f1)
case $GOT in "$WANT"*) echo "blob sha256 ok ($GOT)";; *) echo "blob sha256 mismatch: got $GOT, BLOB.sha256 says $WANT"; exit 1;; esac

mkdir -p $M/native /etc/mali-wl
cp -f "$BLOB" $M/blob.so
for l in libEGL.so libEGL.so.1 libGLESv2.so libGLESv2.so.2 libgbm.so libgbm.so.1 libmali.so.1 libMali.so libwayland-egl.so libwayland-egl.so.1; do
  ln -sf ../blob.so $M/native/$l; done
rm -f $M/mali_shim.so $M/gbm_compat.so $M/fakedrm.so $M/fakeudev.so $M/fakedrm_atomic.so   # replace, never overwrite in place
CF="-shared -fPIC -O2 -Wall -I/usr/include/libdrm"
for s in mali_shim gbm_compat fakedrm fakeudev; do gcc $CF -o $M/$s.so "$HERE/src/$s.c" -ldl; done
[ -s "$HERE/src/fakedrm_atomic.c" ] && gcc $CF -o $M/fakedrm_atomic.so "$HERE/src/fakedrm_atomic.c" -ldl
# Weston's drm-backend needs gbm_bo_get_fd_for_plane, which this blob lacks -> gbm_compat.so supplies it
rm -f $M/use_gbm_compat
nm -D --defined-only $M/blob.so | grep -q ' gbm_bo_get_fd_for_plane$' || touch $M/use_gbm_compat
install -m755 "$HERE"/bin/* /usr/local/bin/
[ -e /etc/mali-wl/weston.ini ] || cp "$HERE/config/weston.ini" /etc/mali-wl/weston.ini
[ -d /etc/systemd/system ] && install -m644 "$HERE/systemd/weston-gpu.service" /etc/systemd/system/
cat <<MSG

Installed to $M.
  Self-test:           sudo $HERE/test/newblob-test.sh --blob $M/blob.so --src $HERE --ignore-leftovers
  Start Weston:        weston-gpu &            (or: systemctl enable --now weston-gpu)
  Run GPU apps:        gpu-run weston-simple-egl      gpu-run firefox-esr --no-remote ...
  mpv without Weston:  mpv-gpu video.mp4
Kernel side must provide: /dev/mali0 (OEM mali.ko), /dev/ion, /dev/fb0.  Run mali-dev-init at every boot.
MSG
