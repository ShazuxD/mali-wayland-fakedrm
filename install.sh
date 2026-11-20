#!/bin/bash
# Install the Mali-G31 GPU userspace stack on a (new) Debian arm64 rootfs.
# Usage: sudo ./install.sh [--blob /path/to/blob.so] [--no-apt]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); M=/opt/mali-wl; BLOB=""; APT=1
while [ $# -gt 0 ]; do case $1 in --blob) BLOB=$2; shift 2;; --no-apt) APT=0; shift;; *) echo "unknown arg $1"; exit 1;; esac; done
[ -n "$BLOB" ] || for c in "$HERE/blob/blob.so" /root/drmprobe/lib/blob.so; do [ -s "$c" ] && BLOB=$c && break; done
[ -s "$BLOB" ] || { echo "OEM Mali blob not found. Pass --blob /path/blob.so (see BLOB.sha256)"; exit 1; }
for f in mali_shim.c gbm_compat.c fakedrm.c fakeudev.c wlegl.c; do
  [ -s "$HERE/src/$f" ] || { echo "missing src/$f (run ./collect.sh on the working box first)"; exit 1; }
done
if [ $APT = 1 ]; then
  apt-get update
  apt-get install -y gcc libc6-dev libwayland-dev libwayland-egl-backend-dev libegl-dev libgbm-dev libdrm-dev \
    weston mpv firefox-esr libwayland-client0
fi
mkdir -p $M/native /etc/mali-wl
cp "$BLOB" $M/blob.so
for l in libEGL.so.1 libGLESv2.so.2 libgbm.so.1 libmali.so.1; do ln -sf ../blob.so $M/native/$l; done   # native blob (Weston, mpv)
for l in libGLESv2.so libGLESv2.so.2 libgbm.so.1 libmali.so.1; do ln -sf blob.so $M/$l; done                         # wrapper dir (gpu-run apps)
rm -f $M/mali_shim.so $M/gbm_compat.so $M/fakedrm.so $M/fakeudev.so $M/fakedrm_atomic.so   # replace, never overwrite in place (running apps map them)
CF="-shared -fPIC -O2 -Wall -I/usr/include/libdrm"
for s in mali_shim gbm_compat fakedrm fakeudev; do gcc $CF -o $M/$s.so "$HERE/src/$s.c" -ldl; done
[ -s "$HERE/src/fakedrm_atomic.c" ] && gcc $CF -o $M/fakedrm_atomic.so "$HERE/src/fakedrm_atomic.c" -ldl
rm -f $M/libEGL.so.1
gcc $CF -Wextra -Wl,-soname,libEGL.so.1 -o $M/libEGL.so.1 "$HERE/src/wlegl.c" -ldl -lwayland-client -lpthread
ln -sf libEGL.so.1 $M/libEGL.so   # Firefox dlopens "libEGL.so" first
install -m755 "$HERE"/bin/* /usr/local/bin/
[ -e /etc/mali-wl/weston.ini ] || cp "$HERE/config/weston.ini" /etc/mali-wl/weston.ini
[ -d /etc/systemd/system ] && install -m644 "$HERE/systemd/weston-gpu.service" /etc/systemd/system/
cat <<MSG

Installed to $M.
  Start Weston:        weston-gpu &            (or: systemctl enable --now weston-gpu)
  Run GPU apps:        gpu-run weston-simple-egl      gpu-run firefox-esr ...
  mpv without Weston:  mpv-gpu video.mp4
Kernel side must provide: /dev/mali0 (OEM mali.ko), /dev/ion, /dev/fb0.  Run mali-dev-init at every boot.
MSG
