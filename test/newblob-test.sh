#!/bin/bash
# Test the libmali-bifrost-g31-g13p0-wayland-gbm.so blob WITHOUT the wlegl EGL wrapper.
#
#   1. scans for leftovers of the old setup (old blob, /opt/mali-wl, launchers, ld.so.preload, fake card0 ...)
#   2. inspects the new blob (arch, deps, exported EGL / GBM / wayland-egl symbols)
#   3. installs blob + shims from the repo zip into /opt/mali-wl (no wlegl, no libEGL wrapper)
#   4. tests: (a) EGL on GBM  (b) EGL on the Wayland platform  (c) weston-simple-egl  (d) optional Firefox
#
# Usage (as root, over SSH is fine):
#   bash mali-newblob-test.sh --blob /path/libmali-bifrost-g31-g13p0-wayland-gbm.so --src /path/mali-wayland-fakedrm-master
#
# Options:
#   --blob FILE       new blob (default: searched in ., /root, /home/*, /mnt, /media, /tmp by file name)
#   --src DIR         unzipped repo (default: ./mali-wayland-fakedrm-master or next to this script)
#   --clean           move leftovers of the old setup to /root/mali-old-<time>/ (nothing is deleted outright)
#   --ignore-leftovers  carry on even though leftovers exist (otherwise the script stops after the scan)
#   --gbm-compat      also preload gbm_compat.so (only if the scan says GBM entry points are missing)
#   --firefox         also try Firefox (WebRender) at the end
#   --keep-weston     leave Weston running after the tests
#   --scan-only       only scan + inspect the blob, change nothing
#
# Logs: /root/mali-test/logs/   Staging copy of blob + repo: /root/mali-newblob/ (never touched by --clean)

M=/opt/mali-wl
STAGE=/root/mali-newblob
T=/root/mali-test
LOGS=$T/logs
BLOBNAME=libmali-bifrost-g31-g13p0-wayland-gbm.so
BLOB=""; SRC=""; CLEAN=0; IGNORE=0; GBMCOMPAT=0; FIREFOX=0; KEEPW=0; SCANONLY=0
while [ $# -gt 0 ]; do case $1 in
  --blob) BLOB=$2; shift 2;; --src) SRC=$2; shift 2;;
  --clean) CLEAN=1; shift;; --ignore-leftovers) IGNORE=1; shift;;
  --gbm-compat) GBMCOMPAT=1; shift;; --firefox) FIREFOX=1; shift;;
  --keep-weston) KEEPW=1; shift;; --scan-only) SCANONLY=1; shift;;
  -h|--help) sed -n '2,24p' "$0"; exit 0;;
  *) echo "unknown arg $1"; exit 1;; esac; done

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
PASS=(); FAIL=(); SKIP=()
hdr(){ printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
ok(){ echo "  [ok]   $*"; }
warn(){ echo "  [warn] $*"; }
bad(){ echo "  [FAIL] $*"; }
pass(){ PASS+=("$1"); ok "TEST $1: PASS"; }
fail(){ FAIL+=("$1"); bad "TEST $1: FAIL  ($2)"; }
skip(){ SKIP+=("$1"); warn "TEST $1: skipped ($2)"; }
mkdir -p "$LOGS" "$STAGE"

# ---------------------------------------------------------------- 0. locate inputs
hdr "0. inputs"
if [ -z "$BLOB" ]; then
  BLOB=$(find . /root /home /mnt /media /tmp -maxdepth 4 -name "$BLOBNAME" -not -path "$STAGE/*" 2>/dev/null | head -1)
  [ -z "$BLOB" ] && [ -s "$STAGE/blob/$BLOBNAME" ] && BLOB=$STAGE/blob/$BLOBNAME
fi
[ -s "$BLOB" ] || { bad "new blob not found. Pass --blob /path/$BLOBNAME"; exit 1; }
if [ -z "$SRC" ]; then
  for c in ./mali-wayland-fakedrm-master "$HERE/mali-wayland-fakedrm-master" "$HERE" "$STAGE/repo"; do
    [ -s "$c/src/fakedrm.c" ] && SRC=$c && break; done
fi
[ -s "$SRC/src/fakedrm.c" ] || { bad "repo not found. Pass --src /path/mali-wayland-fakedrm-master"; exit 1; }
# stage copies first: --clean must never remove the new blob or the sources
mkdir -p "$STAGE/blob"
[ "$(readlink -f "$BLOB")" = "$(readlink -f "$STAGE/blob/$BLOBNAME")" ] || cp -f "$BLOB" "$STAGE/blob/$BLOBNAME"
[ "$(readlink -f "$SRC")" = "$(readlink -f "$STAGE/repo")" ] || { rm -rf "$STAGE/repo"; cp -a "$SRC" "$STAGE/repo"; }
BLOB=$STAGE/blob/$BLOBNAME; SRC=$STAGE/repo
ok "blob: $BLOB ($(stat -c %s "$BLOB") bytes, sha256 $(sha256sum "$BLOB" | cut -c1-16)...)"
ok "repo: $SRC (staged under $STAGE, safe from --clean)"

# ---------------------------------------------------------------- 1. leftovers scan
hdr "1. leftovers of the old setup"
NEWSHA=$(sha256sum "$BLOB" | cut -c1-64)   # copies identical to the new blob are not "old"
LEFT=()
for p in /opt/mali-wl /opt/mali /etc/mali-wl /root/drmprobe /root/ff-gpu /root/blob.so.backup \
         /usr/local/bin/gpu-run /usr/local/bin/weston-gpu /usr/local/bin/mpv-gpu /usr/local/bin/mali-dev-init \
         /usr/local/bin/gpu-top /usr/local/bin/xfce-fb /etc/systemd/system/weston-gpu.service \
         /etc/X11/xorg.conf.d/10-fbdev.conf /etc/X11/xorg.conf.d/20-fbdev.conf; do
  [ -e "$p" ] || [ -L "$p" ] && LEFT+=("$p")
done
[ -s /etc/ld.so.preload ] && LEFT+=(/etc/ld.so.preload)
if [ -c /dev/dri/card0 ] && [ ! -d /sys/class/drm/card0 ]; then LEFT+=(/dev/dri/card0); fi   # node without a real DRM device = fake
while IFS= read -r f; do LEFT+=("$f"); done < <(
  find / -xdev \( -path /proc -o -path /home -o -path "$STAGE" -o -path "$T" -o -path /root/mali-old-\* \) -prune -o -type f \
    \( -name blob.so -o -iname 'libmali*' -o -name 'libwlegl*' -o -name mali_shim.so -o -name 'fakedrm*.so' \
       -o -name fakeudev.so -o -name gbm_compat.so \) -print 2>/dev/null | sort -u |
  while IFS= read -r f; do [ "$(sha256sum "$f" | cut -c1-64)" = "$NEWSHA" ] || echo "$f"; done)
while IFS= read -r f; do LEFT+=("(ldconfig cache) $f"); done < <(ldconfig -p 2>/dev/null | grep -i mali)
# env hooks: reported only, never edited
HOOKS=$(grep -lsE 'mali-wl|drmprobe|gpu-run|LD_PRELOAD' /etc/environment /etc/profile /etc/profile.d/* /root/.bashrc /root/.profile \
          /etc/rc.local /etc/ld.so.conf.d/* /etc/systemd/system/*.service 2>/dev/null | grep -v weston-gpu.service)
PROCS=$(pgrep -a 'weston|Xwayland|firefox|mpv|Xorg' 2>/dev/null)
if [ ${#LEFT[@]} -eq 0 ]; then ok "clean: no old blob, shims, launchers, config or fake card0 found"
else warn "${#LEFT[@]} leftover(s):"; printf '         %s\n' "${LEFT[@]}"; fi
[ -n "$HOOKS" ] && { warn "files that still mention the old setup (NOT edited, check by hand):"; echo "$HOOKS" | sed 's/^/         /'; }
[ -n "$PROCS" ] && { warn "running graphics processes:"; echo "$PROCS" | sed 's/^/         /'; }

if [ ${#LEFT[@]} -gt 0 ] && [ $CLEAN = 1 ] && [ $SCANONLY = 0 ]; then
  BAK=/root/mali-old-$(date +%Y%m%d-%H%M%S); mkdir -p "$BAK"; ok "moving leftovers to $BAK"
  pkill -9 weston 2>/dev/null; pkill -9 Xwayland 2>/dev/null; pkill -9 firefox-esr 2>/dev/null; pkill -9 mpv 2>/dev/null; sleep 1
  systemctl disable --now weston-gpu.service 2>/dev/null
  for p in "${LEFT[@]}"; do
    case $p in "(ldconfig cache)"*) continue;; /dev/dri/card0) rm -f "$p"; continue;; esac
    [ -e "$p" ] || [ -L "$p" ] || continue
    mkdir -p "$BAK$(dirname "$p")"; mv "$p" "$BAK$p" 2>/dev/null && echo "         moved $p"
  done
  ldconfig 2>/dev/null; systemctl daemon-reload 2>/dev/null
  ok "old setup moved away (delete $BAK yourself when satisfied)"
elif [ ${#LEFT[@]} -gt 0 ] && [ $IGNORE = 0 ] && [ $SCANONLY = 0 ]; then
  bad "leftovers present; re-run with --clean (moves them aside) or --ignore-leftovers"; exit 2
fi

# ---------------------------------------------------------------- 2. inspect the new blob
hdr "2. new blob inspection"
FILEINFO=$(file -L "$BLOB"); echo "  $FILEINFO" | cut -c1-200
case $FILEINFO in *"ARM aarch64"*) ok "64-bit aarch64";; *) bad "not aarch64 ELF - wrong blob for this userspace"; exit 1;; esac
echo "  SONAME: $(readelf -d "$BLOB" | awk '/SONAME/{print $5}')   NEEDED: $(readelf -d "$BLOB" | awk '/NEEDED/{gsub(/[\[\]]/,"",$5);printf "%s ",$5}')"
MISSDEP=$(LD_LIBRARY_PATH=$STAGE/blob ldd "$BLOB" 2>&1 | grep 'not found')
[ -n "$MISSDEP" ] && { bad "missing runtime libraries:"; echo "$MISSDEP" | sed 's/^/         /'; echo "         (apt install the matching packages, e.g. libdrm2 libwayland-client0 libwayland-server0)"; } || ok "all shared-library deps resolve"
SYMS=$(nm -D --defined-only "$BLOB" 2>/dev/null | awk '{print $3}')
has(){ echo "$SYMS" | grep -qx "$1"; }
for s in eglGetDisplay eglGetPlatformDisplay eglGetPlatformDisplayEXT eglCreateWindowSurface eglCreatePlatformWindowSurface \
         glGetString gbm_create_device gbm_surface_create gbm_bo_get_fd gbm_bo_map wl_egl_window_create; do
  has $s && ok "exports $s" || warn "no $s"; done
GBMX=""; for s in gbm_bo_get_fd_for_plane gbm_bo_create_with_modifiers2 gbm_surface_create_with_modifiers gbm_surface_create gbm_bo_create; do
  has $s || GBMX="$GBMX $s"; done
[ -n "$GBMX" ] && warn "GBM entry points missing (gbm_compat.so would add them):$GBMX" || ok "all GBM entry points gbm_compat.so supplies are already present"
strings -a "$BLOB" | grep -oE 'EGL_(WL_bind_wayland_display|EXT_platform_wayland|KHR_platform_gbm|EXT_image_dma_buf_import[a-z_]*|KHR_image_base)' | sort -u | sed 's/^/  ext string: /'
strings -a "$BLOB" | grep -m2 -E 'rk_so_ver|g13p0' | sed 's/^/  version: /'
HASWLEGL=0; has wl_egl_window_create && HASWLEGL=1
[ $SCANONLY = 1 ] && { hdr "scan-only: done"; exit 0; }

# ---------------------------------------------------------------- 3. install (no wlegl)
hdr "3. install blob + shims (no wlegl)"
need=""; for p in gcc libc6-dev libwayland-dev libdrm-dev weston; do dpkg -s $p >/dev/null 2>&1 || need="$need $p"; done
if [ -n "$need" ]; then warn "installing:$need"; apt-get install -y $need >"$LOGS/apt.log" 2>&1 || { bad "apt failed, see $LOGS/apt.log"; exit 1; }; fi
mkdir -p $M/native /etc/mali-wl
cp -f "$BLOB" $M/blob.so
for l in libEGL.so libEGL.so.1 libGLESv2.so libGLESv2.so.2 libgbm.so libgbm.so.1 libmali.so.1 libMali.so; do ln -sf ../blob.so $M/native/$l; done
[ $HASWLEGL = 1 ] && for l in libwayland-egl.so libwayland-egl.so.1; do ln -sf ../blob.so $M/native/$l; done
CF="-shared -fPIC -O2 -Wall -I/usr/include/libdrm"
for s in mali_shim fakedrm fakeudev; do
  rm -f $M/$s.so; gcc $CF -o $M/$s.so "$SRC/src/$s.c" -ldl 2>"$LOGS/build-$s.log" && ok "built $s.so" || { bad "build $s failed: $LOGS/build-$s.log"; exit 1; }; done
[ -s "$SRC/src/fakedrm_atomic.c" ] && gcc $CF -o $M/fakedrm_atomic.so "$SRC/src/fakedrm_atomic.c" -ldl 2>"$LOGS/build-atomic.log" && ok "built fakedrm_atomic.so"
# gbm_compat.c hardcodes /root/drmprobe/lib/blob.so -> point it at the new blob
# also: no 1920x1080 mmap diagnostic unless GBM_DIAG=1, and gbm_bo_get_fd_for_plane falls back to gbm_bo_get_fd for plane 0
sed -e "s#/root/drmprobe/lib/blob.so#$M/blob.so#" -e '2i #include <stdlib.h>' \
    -e 's#struct gbm_bo \*bo = lock_fn ? lock_fn(surface) : NULL;#if (!getenv("GBM_DIAG")) return lock_fn ? lock_fn(surface) : NULL;\n    struct gbm_bo *bo = lock_fn ? lock_fn(surface) : NULL;#' \
    -e 's#return real_fn ? real_fn(bo, plane) : -1;#if (real_fn) return real_fn(bo, plane); { int (*g)(struct gbm_bo *) = blob_sym("gbm_bo_get_fd"); return (plane == 0 \&\& g) ? g(bo) : -1; }#' \
    "$SRC/src/gbm_compat.c" > $T/gbm_compat.c
gcc $CF -o $M/gbm_compat.so $T/gbm_compat.c -ldl 2>"$LOGS/build-gbm_compat.log" && ok "built gbm_compat.so (auto-enabled when the blob lacks GBM entry points)"
rm -f $M/use_gbm_compat; if [ $GBMCOMPAT = 1 ] || [ -n "$GBMX" ]; then touch $M/use_gbm_compat; ok "gbm_compat.so ENABLED (Weston drm-backend needs gbm_bo_get_fd_for_plane)"; fi
[ -e /etc/mali-wl/weston.ini ] || cp "$SRC/config/weston.ini" /etc/mali-wl/weston.ini
install -m755 "$SRC/bin/mali-dev-init" /usr/local/bin/mali-dev-init

cat > /usr/local/bin/gpu-run <<'EOF'
#!/bin/sh
# Run a client on the Mali blob's own Wayland/GBM EGL (no wlegl).
M=${MALI_WL_DIR:-/opt/mali-wl}
P=""; [ -e "$M/use_gbm_compat" ] && P="$M/gbm_compat.so:"
export LD_LIBRARY_PATH="$M/native${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_PRELOAD="${P}$M/mali_shim.so:$M/fakedrm.so${LD_PRELOAD:+:$LD_PRELOAD}"
export KBASE_SPOOF=1 FAKE_DRM_OK=1 FAKE_ION_FLAGS=${FAKE_ION_FLAGS:-1}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
if [ -z "$WAYLAND_DISPLAY" ]; then
  for s in "$XDG_RUNTIME_DIR"/wayland-*; do case "$s" in *.lock) ;; *) [ -S "$s" ] && WAYLAND_DISPLAY=${s##*/} && break;; esac; done
  export WAYLAND_DISPLAY
fi
exec "$@"
EOF
cat > /usr/local/bin/weston-gpu <<'EOF'
#!/bin/sh
M=${MALI_WL_DIR:-/opt/mali-wl}
P=""; [ -e "$M/use_gbm_compat" ] && P="$M/gbm_compat.so:"
/usr/local/bin/mali-dev-init
export XDG_RUNTIME_DIR=/run/user/0
exec env LD_LIBRARY_PATH="$M/native" LD_PRELOAD="${P}$M/mali_shim.so:$M/fakedrm.so:$M/fakeudev.so" \
  KBASE_SPOOF=1 FAKE_DRM_OK=1 FAKE_ION_FLAGS=1 \
  weston --backend=drm-backend.so --idle-time=0 --config=/etc/mali-wl/weston.ini "$@"
EOF
chmod 755 /usr/local/bin/gpu-run /usr/local/bin/weston-gpu
mali-dev-init
for n in /dev/mali0 /dev/ion /dev/fb0 /dev/dri/card0; do [ -e $n ] && ok "$n" || warn "$n missing"; done

# ---------------------------------------------------------------- 4. tests
hdr "4. build EGL test program"
cat > $T/mali-egltest.c <<'CEOF'
/* Minimal EGL/GLES2 smoke test that talks straight to the blob (dlopen by soname, like real apps).
 *   mali-egltest gbm       EGL_PLATFORM_GBM_KHR   on /dev/dri/card0
 *   mali-egltest wayland   EGL_PLATFORM_WAYLAND_KHR on $WAYLAND_DISPLAY (bare wl_surface, no shell)  */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int32_t EGLint; typedef unsigned EGLBoolean; typedef unsigned GLenum;
static void *H;
static const char *step = "start";
#define F(ret, name, args) static ret (*name) args
F(void *, eglGetProcAddress, (const char *));
F(EGLDisplay, eglGetPlatformDisplay, (EGLint, void *, const EGLint *));
F(EGLDisplay, eglGetDisplay, (void *));
F(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *));
F(const char *, eglQueryString, (EGLDisplay, EGLint));
F(EGLBoolean, eglBindAPI, (unsigned));
F(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *));
F(EGLBoolean, eglGetConfigAttrib, (EGLDisplay, EGLConfig, EGLint, EGLint *));
F(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *));
F(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, void *, const EGLint *));
F(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext));
F(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface));
F(EGLint, eglGetError, (void));
F(const unsigned char *, glGetString, (GLenum));
F(void, glClearColor, (float, float, float, float));
F(void, glClear, (unsigned));
F(void, glFinish, (void));
F(void, glReadPixels, (int, int, int, int, GLenum, GLenum, void *));
F(void *, gbm_create_device, (int));
F(void *, gbm_surface_create, (void *, uint32_t, uint32_t, uint32_t, uint32_t));
F(void *, gbm_surface_lock_front_buffer, (void *));
F(void, gbm_surface_release_buffer, (void *, void *));
F(uint32_t, gbm_bo_get_stride, (void *));
F(void *, wl_egl_window_create, (struct wl_surface *, int, int));

static void on_alarm(int s) { (void)s; char b[128]; int n = snprintf(b, sizeof b, "[egltest] TIMEOUT (hang) at step: %s\n", step); write(2, b, n); _exit(3); }
#define SYM(n) do { n = dlsym(H, #n); } while (0)
#define NEED(n) do { SYM(n); if (!n) { fprintf(stderr, "[egltest] blob lacks %s\n", #n); return 1; } } while (0)

static struct wl_compositor *comp;
static void reg(void *d, struct wl_registry *r, uint32_t id, const char *i, uint32_t v) {
    (void)d; if (!strcmp(i, "wl_compositor")) comp = wl_registry_bind(r, id, &wl_compositor_interface, v < 4 ? v : 4);
}
static void regrm(void *d, struct wl_registry *r, uint32_t id) { (void)d; (void)r; (void)id; }
static const struct wl_registry_listener rl = { reg, regrm };

static int run(EGLDisplay dpy, void *win, uint32_t want_visual) {
    EGLint maj = 0, min = 0;
    step = "eglInitialize";
    if (!eglInitialize(dpy, &maj, &min)) { printf("[egltest] eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    printf("[egltest] EGL %d.%d vendor=%s version=%s\n", maj, min, eglQueryString(dpy, 0x3053), eglQueryString(dpy, 0x3054));
    printf("[egltest] EGL ext: %s\n", eglQueryString(dpy, 0x3055));
    eglBindAPI(0x30A0);
    step = "eglChooseConfig";
    EGLint ca[] = { 0x3033, 4, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3038 };
    EGLConfig cfgs[64]; EGLint n = 0, pick = -1;
    if (!eglChooseConfig(dpy, ca, cfgs, 64, &n) || n < 1) { printf("[egltest] no config (n=%d err=0x%x)\n", n, eglGetError()); return 1; }
    for (int i = 0; i < n && pick < 0; i++) {
        EGLint v = 0, a = 0; eglGetConfigAttrib(dpy, cfgs[i], 0x302E, &v); eglGetConfigAttrib(dpy, cfgs[i], 0x3021, &a);
        if (!want_visual || (uint32_t)v == want_visual) pick = i;
    }
    printf("[egltest] %d configs, using #%d\n", n, pick);
    if (pick < 0) { puts("[egltest] no config matches the native visual"); return 1; }
    step = "eglCreateContext";
    EGLint c3[] = { 0x3098, 3, 0x3038 }, c2[] = { 0x3098, 2, 0x3038 };
    EGLContext ctx = eglCreateContext(dpy, cfgs[pick], 0, c3);
    if (!ctx) { printf("[egltest] ES3 context failed 0x%x, trying ES2\n", eglGetError()); ctx = eglCreateContext(dpy, cfgs[pick], 0, c2); }
    if (!ctx) { printf("[egltest] eglCreateContext failed 0x%x\n", eglGetError()); return 1; }
    step = "eglCreateWindowSurface";
    EGLSurface sur = eglCreateWindowSurface(dpy, cfgs[pick], win, NULL);
    if (!sur) { printf("[egltest] eglCreateWindowSurface failed 0x%x\n", eglGetError()); return 1; }
    step = "eglMakeCurrent";
    if (!eglMakeCurrent(dpy, sur, sur, ctx)) { printf("[egltest] eglMakeCurrent failed 0x%x\n", eglGetError()); return 1; }
    printf("[egltest] GL_VENDOR=%s\n[egltest] GL_RENDERER=%s\n[egltest] GL_VERSION=%s\n",
           glGetString(0x1F00), glGetString(0x1F01), glGetString(0x1F02));
    static const float col[3][3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
    int bad = 0;
    for (int f = 0; f < 6; f++) {
        const float *c = col[f % 3];
        step = "glClear/glFinish"; glClearColor(c[0], c[1], c[2], 1); glClear(0x4000); glFinish();
        unsigned char px[4] = {0}; step = "glReadPixels"; glReadPixels(128, 128, 1, 1, 0x1908, 0x1401, px);
        int okp = (px[0] > 200) == (c[0] > .5) && (px[1] > 200) == (c[1] > .5) && (px[2] > 200) == (c[2] > .5);
        printf("[egltest] frame %d expect %d%d%d got %u,%u,%u %s\n", f, (int)c[0], (int)c[1], (int)c[2], px[0], px[1], px[2], okp ? "ok" : "MISMATCH");
        bad += !okp;
        step = "eglSwapBuffers"; if (!eglSwapBuffers(dpy, sur)) { printf("[egltest] eglSwapBuffers failed 0x%x\n", eglGetError()); return 1; }
        if (win && gbm_surface_lock_front_buffer && want_visual) {
            step = "gbm lock_front_buffer"; void *bo = gbm_surface_lock_front_buffer((void *)win);
            printf("[egltest] gbm front bo=%p stride=%u\n", bo, bo && gbm_bo_get_stride ? gbm_bo_get_stride(bo) : 0);
            if (bo) gbm_surface_release_buffer((void *)win, bo);
        }
    }
    return bad ? 4 : 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "gbm";
    const char *lib = getenv("MALI_EGL_LIB") ? getenv("MALI_EGL_LIB") : "libEGL.so.1";
    signal(SIGALRM, on_alarm); alarm(30);
    H = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!H) { fprintf(stderr, "[egltest] dlopen(%s): %s\n", lib, dlerror()); return 1; }
    Dl_info di; if (dladdr(dlsym(H, "eglInitialize"), &di)) printf("[egltest] eglInitialize comes from %s\n", di.dli_fname);
    NEED(eglInitialize); NEED(eglQueryString); NEED(eglBindAPI); NEED(eglChooseConfig); NEED(eglGetConfigAttrib);
    NEED(eglCreateContext); NEED(eglCreateWindowSurface); NEED(eglMakeCurrent); NEED(eglSwapBuffers); NEED(eglGetError);
    NEED(glGetString); NEED(glClearColor); NEED(glClear); NEED(glFinish); NEED(glReadPixels);
    SYM(eglGetProcAddress); SYM(eglGetPlatformDisplay); SYM(eglGetDisplay);
    SYM(gbm_create_device); SYM(gbm_surface_create); SYM(gbm_surface_lock_front_buffer);
    SYM(gbm_surface_release_buffer); SYM(gbm_bo_get_stride); SYM(wl_egl_window_create);
    if (!eglGetPlatformDisplay && eglGetProcAddress) eglGetPlatformDisplay = eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!eglGetPlatformDisplay) { puts("[egltest] no eglGetPlatformDisplay(EXT) in blob"); return 1; }

    if (!strcmp(mode, "gbm")) {
        if (!gbm_create_device || !gbm_surface_create) { puts("[egltest] blob has no GBM"); return 1; }
        step = "open card0"; int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (fd < 0) { perror("[egltest] open /dev/dri/card0"); return 1; }
        step = "gbm_create_device"; void *dev = gbm_create_device(fd);
        if (!dev) { puts("[egltest] gbm_create_device failed"); return 1; }
        uint32_t fmts[] = { 0x34325258 /*XR24*/, 0x34325241 /*AR24*/ };
        for (int i = 0; i < 2; i++) {
            step = "gbm_surface_create"; void *gs = gbm_surface_create(dev, 256, 256, fmts[i], 5 /*SCANOUT|RENDERING*/);
            if (!gs) { printf("[egltest] gbm_surface_create fmt 0x%08x failed\n", fmts[i]); continue; }
            step = "eglGetPlatformDisplay(gbm)"; EGLDisplay d = eglGetPlatformDisplay(0x31D7, dev, NULL);
            if (!d) { printf("[egltest] eglGetPlatformDisplay(GBM) failed 0x%x\n", eglGetError()); return 1; }
            int r = run(d, gs, fmts[i]);
            if (r == 0 || i == 1) return r;
            printf("[egltest] retrying with next format\n");
        }
        return 1;
    }
    /* wayland */
    step = "wl_display_connect"; struct wl_display *wd = wl_display_connect(NULL);
    if (!wd) { puts("[egltest] wl_display_connect failed (is Weston running? WAYLAND_DISPLAY set?)"); return 1; }
    struct wl_registry *r = wl_display_get_registry(wd); wl_registry_add_listener(r, &rl, NULL);
    wl_display_roundtrip(wd);
    if (!comp) { puts("[egltest] no wl_compositor"); return 1; }
    struct wl_surface *s = wl_compositor_create_surface(comp);
    if (!wl_egl_window_create) { puts("[egltest] blob has no wl_egl_window_create"); return 1; }
    void *win = wl_egl_window_create(s, 256, 256);
    step = "eglGetPlatformDisplay(wayland)"; EGLDisplay d = eglGetPlatformDisplay(0x31D8, wd, NULL);
    if (!d) { printf("[egltest] eglGetPlatformDisplay(WAYLAND) failed 0x%x\n", eglGetError()); return 1; }
    return run(d, win, 0);
}
CEOF
gcc -O1 -Wall -Wno-unused-result -o $T/mali-egltest $T/mali-egltest.c -ldl -lwayland-client 2>$LOGS/build-egltest.log \
  && ok "built $T/mali-egltest" || { bad "egltest build failed:"; cat $LOGS/build-egltest.log; exit 1; }

hdr "4a. EGL on GBM (/dev/dri/card0 via fakedrm, no compositor needed)"
pkill -0 weston 2>/dev/null && warn "Weston already running; fine, GBM test does not need it stopped"
timeout 45 gpu-run $T/mali-egltest gbm > $LOGS/t-gbm.log 2>&1; rc=$?
grep -E 'egltest|shim|gbm|drm' $LOGS/t-gbm.log | head -30
[ $rc = 0 ] && pass gbm-egl || fail gbm-egl "exit $rc, see $LOGS/t-gbm.log"

hdr "4b. Wayland platform of the new blob"
STARTED=0
if ! ls /run/user/0/wayland-* >/dev/null 2>&1 || ! pgrep -x weston >/dev/null; then
  ok "starting weston-gpu"; (nohup weston-gpu > $LOGS/weston.log 2>&1 &); STARTED=1
  for i in $(seq 1 20); do sleep 1; ls /run/user/0/wayland-?  >/dev/null 2>&1 && break; done
fi
if ls /run/user/0/wayland-? >/dev/null 2>&1 && pgrep -x weston >/dev/null; then
  ok "Weston up: $(cd /run/user/0 && ls wayland-? | tr '\n' ' ')"
  timeout 45 gpu-run $T/mali-egltest wayland > $LOGS/t-wayland.log 2>&1; rc=$?
  grep -E 'egltest|shim' $LOGS/t-wayland.log | head -30
  [ $rc = 0 ] && pass wayland-egl || fail wayland-egl "exit $rc, see $LOGS/t-wayland.log"

  hdr "4c. weston-simple-egl (real shell surface, 10 s)"
  if command -v weston-simple-egl >/dev/null; then
    timeout -s INT 14 stdbuf -oL -eL gpu-run weston-simple-egl > $LOGS/t-simple.log 2>&1; rc=$?   # stdbuf: stdout to a file is block-buffered, the FPS line would be lost on kill
    tail -6 $LOGS/t-simple.log
    if grep -qE '[0-9]+ frames in' $LOGS/t-simple.log; then pass simple-egl
    else fail simple-egl "no FPS lines (rc=$rc), see $LOGS/t-simple.log"
         echo "         --- weston.log tail ---"; tail -15 $LOGS/weston.log | sed 's/^/         /'
         echo "         --- compositor still alive? $(pgrep -x weston >/dev/null && echo yes || echo NO - it crashed)"; fi
  else skip simple-egl "weston-simple-egl not installed"; fi

  if [ $FIREFOX = 1 ]; then
    hdr "4d. Firefox (native Wayland EGL, no wrapper)"
    if command -v firefox-esr >/dev/null; then
      pkill -9 firefox-esr 2>/dev/null; sleep 1
      (MOZ_ENABLE_WAYLAND=1 MOZ_WEBRENDER=1 gpu-run firefox-esr --no-remote --profile /root/ff-gpu about:support > $LOGS/t-firefox.log 2>&1 &)
      sleep 45
      grep -E 'GFX1|EGL' $LOGS/t-firefox.log | grep -v glxtest | tail -15
      if grep -qE 'Failed|Fallback' $LOGS/t-firefox.log; then fail firefox "GFX failure lines, see $LOGS/t-firefox.log"
      else pass firefox; warn "confirm in about:support that Compositing says 'WebRender' and not 'WebRender (Software)'"; fi
      pkill -9 firefox-esr 2>/dev/null
    else skip firefox "firefox-esr not installed"; fi
  fi
  if [ $STARTED = 1 ] && [ $KEEPW = 0 ]; then pkill -x weston; sleep 1; ok "stopped Weston (use --keep-weston to leave it up)"; fi
else
  fail wayland-egl "Weston did not start"; tail -25 $LOGS/weston.log | sed 's/^/         /'
  skip simple-egl "no Weston"; [ $FIREFOX = 1 ] && skip firefox "no Weston"
fi

hdr "SUMMARY"
for t in "${PASS[@]}"; do echo "  PASS  $t"; done
for t in "${SKIP[@]}"; do echo "  SKIP  $t"; done
for t in "${FAIL[@]}"; do echo "  FAIL  $t"; done
echo; echo "Logs: $LOGS/   If anything failed, paste: tail -40 on the failing t-*.log (and weston.log)"
echo "Extra detail:  KBASE_DEBUG=1 gpu-run $T/mali-egltest gbm"
[ ${#FAIL[@]} -eq 0 ]
