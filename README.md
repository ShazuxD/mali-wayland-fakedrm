# Mali-G31 GPU userspace on Debian arm64 (kernel 4.9, no DRM)

Userspace-only GPU stack for Amlogic/Rockchip boxes whose stock kernel has the OEM Mali (kbase UK 11.21) but no DRM/KMS.

| Piece | What it does |
|---|---|
| `mali_shim.so` | Kbase ABI shim: fakes the 11.35 handshake, repacks 64->56 byte atoms |
| `gbm_compat.so` | adds missing GBM entry points to the OEM blob |
| `fakedrm.so` / `fakedrm_atomic.so` | fake `/dev/dri/card0` (GEM/PRIME on ION, legacy or atomic KMS, flips copied to `/dev/fb0`) |
| `fakeudev.so` | synthetic udev DRM device for Weston |
| `libEGL.so.1` (wlegl) | EGL wrapper adding the Wayland platform for apps (renders via GBM, copies to `wl_shm`) |
| `weston-gpu`, `gpu-run`, `mpv-gpu`, `mali-dev-init` | launchers (installed to `/usr/local/bin`) |

## Make the repo (on the working box)
    ./collect.sh                  # pulls sources from /root/drmprobe, writes BLOB.sha256
    git init && git add -A && git commit -m init
`blob.so` (OEM proprietary Mali userspace, `g13p0`) is git-ignored: keep your own copy.

## Install on a new rootfs
    sudo ./install.sh --blob /path/to/blob.so
    weston-gpu &                  # then, from the Weston session or SSH (XDG_RUNTIME_DIR=/run/user/0):
    gpu-run weston-simple-egl
    gpu-run firefox-esr --no-remote --profile /root/ff-gpu

The new rootfs needs from the kernel/device: `/dev/mali0` (OEM `mali.ko` loaded), `/dev/ion`, `/dev/fb0`.
Run `mali-dev-init` at every boot (creates `/dev/dri/card0`); `systemd/weston-gpu.service` does it for you if you use systemd.
Status and limits: GPU rendering works; V4L2 hardware decode is unresolved; Weston and `mpv-gpu` cannot run at the same time.
