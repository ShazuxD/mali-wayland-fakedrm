# Mali-G31 GPU userspace on Debian arm64 (kernel 4.9, no DRM)

Userspace-only GPU stack for Amlogic/Rockchip boxes whose stock kernel has the OEM Mali (kbase UK 11.21) but no DRM/KMS.
Uses the Rockchip libmali **`libmali-bifrost-g31-g13p0-wayland-gbm.so`** blob: it brings its own Wayland and GBM EGL,
so apps and Weston talk to it directly. (The old `wlegl` EGL wrapper that copied frames through `wl_shm` is gone.)

| Piece | What it does |
|---|---|
| `blob.so` (libmali wayland-gbm) | EGL/GLES 3.2/GBM/wayland-egl, symlinked as `libEGL.so.1`, `libGLESv2.so.2`, `libgbm.so.1`, `libwayland-egl.so.1` in `/opt/mali-wl/native` |
| `mali_shim.so` | Kbase ABI shim: fakes the 11.35 handshake (blob wants 11.35, kernel is 11.21), repacks 64->56 byte atoms |
| `gbm_compat.so` | adds `gbm_bo_get_fd_for_plane` / `gbm_bo_create_with_modifiers2` etc. Needed by Weston's drm-backend; enabled automatically when the blob lacks them |
| `fakedrm.so` / `fakedrm_atomic.so` | fake `/dev/dri/card0` (GEM/PRIME on ION, legacy or atomic KMS, flips copied to `/dev/fb0`) |
| `fakeudev.so` | synthetic udev DRM device for Weston |
| `weston-gpu`, `gpu-run`, `mpv-gpu`, `mali-dev-init` | launchers (installed to `/usr/local/bin`) |

## Install
    sudo ./install.sh                       # downloads the pinned blob (sha256 checked), builds shims, installs launchers
    sudo ./install.sh --blob /path/libmali-bifrost-g31-g13p0-wayland-gbm.so   # or use your own copy
    weston-gpu &                            # then, from the Weston session or SSH (XDG_RUNTIME_DIR=/run/user/0):
    gpu-run weston-simple-egl
    gpu-run firefox-esr --no-remote --profile /root/ff-gpu

The blob is proprietary [ARM/Rockchip](https://github.com/jethome-iot/rockchip-libmali)  and a copy is stored in `blobs`

The box needs from the kernel/device: `/dev/mali0` (OEM `mali.ko` loaded), `/dev/ion`, `/dev/fb0`.
Run `mali-dev-init` at every boot (creates `/dev/dri/card0`); `systemd/weston-gpu.service` does it for you if you use systemd.

## Self-test
    sudo test/newblob-test.sh --blob /opt/mali-wl/blob.so --src . --ignore-leftovers [--firefox]
Runs EGL on GBM, EGL on the Wayland platform, and `weston-simple-egl`; logs go to `/root/mali-test/logs`.
Use `--clean` on a box that still has the old wlegl setup (moves leftovers aside, deletes nothing).

## Status
Verified on the box: GBM EGL, native Wayland EGL, `weston-simple-egl` at ~60 fps (Mali-G31, GLES 3.2), smooth rotating test pyramid on the monitor.
