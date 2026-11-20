/* wlegl.c - libEGL.so.1 wrapper that adds the Wayland platform on top of the Mali
 * GBM-only blob.  Apps (Firefox, weston-simple-egl, glmark2...) see a normal
 * EGL_KHR_platform_wayland.  Internally:
 *   - one real EGLDisplay on a gbm_device (fake DRM node + ION buffers, as in Weston)
 *   - each wl_egl_window gets a gbm_surface + real EGLSurface
 *   - eglSwapBuffers: real swap -> glFinish -> map the front bo -> copy into a wl_shm
 *     buffer -> attach/damage/commit on the app's wl_surface
 * Needs LD_PRELOAD of gbm_compat.so, mali_shim.so, fakedrm.so (see gpu-run).
 * Env: WLEGL_REAL=/path/blob.so  WLEGL_DEBUG=1  WLEGL_NOFINISH=1  WLEGL_DRM_NODE=/dev/dri/card0
 */
#define _GNU_SOURCE
#define EGL_NO_X11 1
#define MESA_EGL_NO_X11_HEADERS 1
#define EGL_EGLEXT_PROTOTYPES 1
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <wayland-client.h>
#include <wayland-egl-backend.h>

#define NBUF 3
#define MAGIC 0x574c4547u

static pthread_mutex_t mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static void *real_lib, *self;
static int dbg;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static __thread EGLint werr;
#define LOG(...) do { if (dbg) { fprintf(stderr, "[wlegl] " __VA_ARGS__); } } while (0)

/* ---------- real-library access ---------- */
static void *R(const char *n);
static void *G(const char *n) {            /* gbm: prefer preloaded wrappers (gbm_compat) */
    void *p = dlsym(RTLD_DEFAULT, n);
    return p ? p : (real_lib ? dlsym(real_lib, n) : NULL);
}
static struct {
    struct gbm_device *(*create)(int);
    struct gbm_surface *(*screate)(struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*sdestroy)(struct gbm_surface *);
    struct gbm_bo *(*lock)(struct gbm_surface *);
    void (*release)(struct gbm_surface *, struct gbm_bo *);
    uint32_t (*stride)(struct gbm_bo *);
    uint32_t (*format)(struct gbm_bo *);
    int (*get_fd)(struct gbm_bo *);
    void *(*map)(struct gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **);
    void (*unmap)(struct gbm_bo *, void *);
} gb;
static void init_real(void) {
    const char *p = getenv("WLEGL_REAL");
    char buf[1024];
    dbg = getenv("WLEGL_DEBUG") != NULL;
    { Dl_info d2; if (dladdr((void *)init_real, &d2) && d2.dli_fname) self = dlopen(d2.dli_fname, RTLD_NOW | RTLD_NOLOAD); }
    if (!p) {
        Dl_info di;
        if (dladdr((void *)init_real, &di) && di.dli_fname) {
            strncpy(buf, di.dli_fname, sizeof buf - 16); buf[sizeof buf - 16] = 0;
            char *sl = strrchr(buf, '/');
            if (sl) { strcpy(sl + 1, "blob.so"); p = buf; }
        }
        if (!p) p = "blob.so";
    }
    /* GLOBAL so RTLD_NEXT lookups from the preloaded gbm_compat/shims can find the blob's gbm */
    real_lib = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
    if (!real_lib) { fprintf(stderr, "[wlegl] cannot load real EGL blob %s: %s\n", p, dlerror()); return; }
    gb.create = G("gbm_create_device");   gb.screate = G("gbm_surface_create");
    gb.sdestroy = G("gbm_surface_destroy"); gb.lock = G("gbm_surface_lock_front_buffer");
    gb.release = G("gbm_surface_release_buffer"); gb.stride = G("gbm_bo_get_stride");
    gb.format = G("gbm_bo_get_format"); gb.get_fd = G("gbm_bo_get_fd");
    gb.map = G("gbm_bo_map"); gb.unmap = G("gbm_bo_unmap");
    LOG("real blob %s loaded; gbm_bo_get_fd=%s gbm_bo_map=%s\n", p, gb.get_fd ? "yes" : "no", gb.map ? "yes" : "no");
}
static void *R(const char *n) { pthread_once(&once, init_real); return real_lib ? dlsym(real_lib, n) : NULL; }
#define REAL(fn) ((__typeof__(&fn))R(#fn))

/* ---------- state ---------- */
struct wbuf { struct wl_buffer *buf; void *data; size_t size; int busy; };
struct wsurf {
    uint32_t magic;
    struct wl_egl_window *win;
    EGLConfig cfg; EGLint vid;
    struct gbm_surface *gs; EGLSurface inner;
    int w, h;
    struct wbuf b[NBUF];
    struct wsurf *next;
};
static struct wsurf *surfs;
static __thread struct wsurf *cur_draw, *cur_read;
static EGLDisplay inner_dpy = EGL_NO_DISPLAY;
static struct gbm_device *gdev;
static struct wl_display *wld;
static struct wl_event_queue *wq;
static struct wl_shm *shm;

static struct wsurf *wfind(EGLSurface h) {
    struct wsurf *s;
    if (h == EGL_NO_SURFACE) return NULL;
    pthread_mutex_lock(&mu);
    for (s = surfs; s; s = s->next) if ((EGLSurface)s == h) break;
    pthread_mutex_unlock(&mu);
    return s;
}
static EGLSurface T(EGLSurface h) { struct wsurf *s = wfind(h); return s ? s->inner : h; }

static EGLDisplay get_inner(void) {
    pthread_mutex_lock(&mu);
    if (inner_dpy == EGL_NO_DISPLAY) {
        const char *node = getenv("WLEGL_DRM_NODE") ? getenv("WLEGL_DRM_NODE") : "/dev/dri/card0";
        R("eglGetError");
        if (!real_lib || !gb.create) { fprintf(stderr, "[wlegl] real blob/gbm not available\n"); goto out; }
        int fd = open(node, O_RDWR | O_CLOEXEC);
        if (fd < 0) { fprintf(stderr, "[wlegl] open %s failed (is fakedrm.so in LD_PRELOAD? use gpu-run)\n", node); goto out; }
        gdev = gb.create(fd);
        if (!gdev) { fprintf(stderr, "[wlegl] gbm_create_device failed\n"); goto out; }
        PFNEGLGETPROCADDRESSPROC gpa = (PFNEGLGETPROCADDRESSPROC)R("eglGetProcAddress");
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = gpa ? (PFNEGLGETPLATFORMDISPLAYEXTPROC)gpa("eglGetPlatformDisplayEXT") : NULL;
        EGLDisplay d = gpd ? gpd(EGL_PLATFORM_GBM_KHR, gdev, NULL) : EGL_NO_DISPLAY;
        if (d == EGL_NO_DISPLAY) d = REAL(eglGetDisplay)((EGLNativeDisplayType)gdev);
        inner_dpy = d;
        LOG("inner display %p on gbm device %p\n", (void *)d, (void *)gdev);
    }
out:
    pthread_mutex_unlock(&mu);
    return inner_dpy;
}

/* ---------- wayland side ---------- */
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d; (void)ver;
    if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };
static void buf_release(void *data, struct wl_buffer *b) { (void)b; ((struct wbuf *)data)->busy = 0; }
static const struct wl_buffer_listener buf_l = { buf_release };

static int wl_setup(void) {
    if (shm) return 1;
    if (!wld) return 0;
    wq = wl_display_create_queue(wld);
    struct wl_display *w = wl_proxy_create_wrapper(wld);
    wl_proxy_set_queue((struct wl_proxy *)w, wq);
    struct wl_registry *reg = wl_display_get_registry(w);
    wl_proxy_wrapper_destroy(w);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip_queue(wld, wq);
    wl_display_roundtrip_queue(wld, wq);
    LOG("wl_shm %s\n", shm ? "bound" : "MISSING");
    return shm != NULL;
}
static void free_buffers(struct wsurf *s) {
    for (int i = 0; i < NBUF; i++) {
        if (s->b[i].buf) wl_buffer_destroy(s->b[i].buf);
        if (s->b[i].data) munmap(s->b[i].data, s->b[i].size);
        memset(&s->b[i], 0, sizeof s->b[i]);
    }
}
static int alloc_buffers(struct wsurf *s) {
    size_t size = (size_t)s->w * 4 * s->h;
    for (int i = 0; i < NBUF; i++) {
        int fd = memfd_create("wlegl", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); free_buffers(s); return -1; }
        void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { close(fd); free_buffers(s); return -1; }
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
        s->b[i].buf = wl_shm_pool_create_buffer(pool, 0, s->w, s->h, s->w * 4, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        s->b[i].data = m; s->b[i].size = size; s->b[i].busy = 0;
        wl_buffer_add_listener(s->b[i].buf, &buf_l, &s->b[i]);
    }
    return 0;
}
static struct wbuf *acquire(struct wsurf *s) {
    int waits = 0;
    for (;;) {
        wl_display_dispatch_queue_pending(wld, wq);
        for (int i = 0; i < NBUF; i++) if (!s->b[i].busy) return &s->b[i];
        if (wl_display_prepare_read_queue(wld, wq) != 0) { wl_display_dispatch_queue_pending(wld, wq); continue; }
        wl_display_flush(wld);
        struct pollfd p = { wl_display_get_fd(wld), POLLIN, 0 };
        int r = poll(&p, 1, 100);
        if (r > 0) wl_display_read_events(wld); else wl_display_cancel_read(wld);
        if (++waits > 30) { LOG("no free shm buffer after 3 s, reusing one\n"); return &s->b[0]; }
    }
}

/* ---------- gbm / inner surface ---------- */
static int make_inner(EGLDisplay d, struct wsurf *s, int w, int h) {
    uint32_t fmt = s->vid ? (uint32_t)s->vid : GBM_FORMAT_XRGB8888;
    s->gs = gb.screate(gdev, w, h, fmt, GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!s->gs) { LOG("gbm_surface_create %dx%d fmt 0x%x failed\n", w, h, fmt); return -1; }
    PFNEGLGETPROCADDRESSPROC gpa = (PFNEGLGETPROCADDRESSPROC)R("eglGetProcAddress");
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC cps = gpa ? (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)gpa("eglCreatePlatformWindowSurfaceEXT") : NULL;
    s->inner = cps ? cps(d, s->cfg, s->gs, NULL) : EGL_NO_SURFACE;
    if (s->inner == EGL_NO_SURFACE) s->inner = REAL(eglCreateWindowSurface)(d, s->cfg, (EGLNativeWindowType)s->gs, NULL);
    if (s->inner == EGL_NO_SURFACE) { LOG("real window surface failed 0x%x\n", REAL(eglGetError)()); gb.sdestroy(s->gs); s->gs = NULL; return -1; }
    LOG("inner surface %dx%d visual 0x%x\n", w, h, fmt);
    return 0;
}
struct dsync { uint64_t flags; };
#define DMABUF_SYNC _IOW('b', 0, struct dsync)

static void present(struct wsurf *s) {
    struct gbm_bo *bo = gb.lock(s->gs);
    if (!bo) { LOG("lock_front_buffer failed\n"); return; }
    uint32_t stride = gb.stride(bo), fmt = gb.format ? gb.format(bo) : GBM_FORMAT_XRGB8888;
    int w = s->w, h = s->h, fd = -1, mapped = 0;
    void *src = NULL, *md = NULL;
    size_t len = (size_t)stride * h;
    if (gb.get_fd && (fd = gb.get_fd(bo)) >= 0) {
        src = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
        if (src == MAP_FAILED) { LOG("mmap of bo fd failed errno %d\n", errno); src = NULL; }
        else { struct dsync sy = { 1 }; ioctl(fd, DMABUF_SYNC, &sy); mapped = 1; }
    }
    if (!src && gb.map) { uint32_t ms = 0; src = gb.map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &ms, &md); if (src) stride = ms; }
    if (src) {
        struct wbuf *b = acquire(s);
        int swz = (fmt == GBM_FORMAT_XBGR8888 || fmt == GBM_FORMAT_ABGR8888);
        for (int y = 0; y < h; y++) {
            const uint32_t *sp = (const uint32_t *)((const char *)src + (size_t)y * stride);
            uint32_t *dp = (uint32_t *)((char *)b->data + (size_t)y * w * 4);
            if (swz) for (int x = 0; x < w; x++) { uint32_t v = sp[x]; dp[x] = (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16); }
            else memcpy(dp, sp, (size_t)w * 4);
        }
        struct wl_surface *ws = s->win->surface;
        wl_surface_attach(ws, b->buf, 0, 0);
        if (wl_proxy_get_version((struct wl_proxy *)ws) >= 4) wl_surface_damage_buffer(ws, 0, 0, w, h);
        else wl_surface_damage(ws, 0, 0, w, h);
        wl_surface_commit(ws);
        b->busy = 1;
        wl_display_flush(wld);
    } else LOG("could not map front buffer\n");
    if (mapped) { struct dsync sy = { 1 | 4 }; ioctl(fd, DMABUF_SYNC, &sy); munmap(src, len); }
    if (fd >= 0) close(fd);
    if (md && gb.unmap) gb.unmap(bo, md);
    gb.release(s->gs, bo);
}
static void resize_surf(EGLDisplay d, struct wsurf *s, int w, int h) {
    struct gbm_surface *ogs = s->gs; EGLSurface oin = s->inner;
    LOG("resize %dx%d -> %dx%d\n", s->w, s->h, w, h);
    if (make_inner(d, s, w, h) < 0) { s->gs = ogs; s->inner = oin; return; }
    EGLContext ctx = REAL(eglGetCurrentContext)();
    if (cur_draw == s || cur_read == s) {
        EGLSurface dr = REAL(eglGetCurrentSurface)(EGL_DRAW), rd = REAL(eglGetCurrentSurface)(EGL_READ);
        REAL(eglMakeCurrent)(d, dr == oin ? s->inner : dr, rd == oin ? s->inner : rd, ctx);
    }
    REAL(eglDestroySurface)(d, oin);
    gb.sdestroy(ogs);
    free_buffers(s);
    s->w = w; s->h = h;
    alloc_buffers(s);
}

/* ---------- wrapped EGL entry points ---------- */
EGLint eglGetError(void) {
    if (werr) { EGLint e = werr; werr = 0; return e; }
    return REAL(eglGetError)();
}
static int looks_like_wl_display(void *n) { return n && *(const void *const *)n == (const void *)&wl_display_interface; }

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attribs) {
    if (platform == EGL_PLATFORM_WAYLAND_KHR) {
        if (native != (void *)wld) { shm = NULL; wq = NULL; wld = native; }
        return get_inner();
    }
    PFNEGLGETPROCADDRESSPROC gpa = (PFNEGLGETPROCADDRESSPROC)R("eglGetProcAddress");
    PFNEGLGETPLATFORMDISPLAYEXTPROC f = gpa ? (PFNEGLGETPLATFORMDISPLAYEXTPROC)gpa("eglGetPlatformDisplayEXT") : NULL;
    return f ? f(platform, native, attribs) : EGL_NO_DISPLAY;
}
EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs) {
    (void)attribs;
    return eglGetPlatformDisplayEXT(platform, native, NULL);
}
EGLDisplay eglGetDisplay(EGLNativeDisplayType n) {
    if (n == EGL_DEFAULT_DISPLAY) return get_inner();
    if (looks_like_wl_display((void *)n)) return eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR, (void *)n, NULL);
    return REAL(eglGetDisplay)(n);
}
static void addext(char *buf, size_t n, const char *e) { if (!strstr(buf, e)) { strncat(buf, buf[0] ? " " : "", n - strlen(buf) - 1); strncat(buf, e, n - strlen(buf) - 1); } }
const char *eglQueryString(EGLDisplay d, EGLint name) {
    const char *r = REAL(eglQueryString)(d, name);
    if (d == EGL_NO_DISPLAY && name == EGL_EXTENSIONS) {
        static char buf[2048];
        snprintf(buf, sizeof buf, "%s", r ? r : "");
        addext(buf, sizeof buf, "EGL_EXT_client_extensions");
        addext(buf, sizeof buf, "EGL_EXT_platform_base");
        addext(buf, sizeof buf, "EGL_KHR_platform_wayland");
        addext(buf, sizeof buf, "EGL_EXT_platform_wayland");
        return buf;
    }
    if (d != EGL_NO_DISPLAY && name == EGL_EXTENSIONS && r) {
        static __thread char b2[4096];
        char *p;
        snprintf(b2, sizeof b2, "%s", r);
        if ((p = strstr(b2, "EGL_EXT_create_context_robustness"))) {   /* blob rejects the attribs it implies */
            size_t n = strlen("EGL_EXT_create_context_robustness");
            if (p[n] == ' ') n++;
            memmove(p, p + n, strlen(p + n) + 1);
        }
        return b2;
    }
    return r;
}
static EGLSurface create_wsurf(EGLDisplay d, EGLConfig cfg, void *native) {
    struct wl_egl_window *win = native;
    if (!win || !win->surface) { werr = EGL_BAD_NATIVE_WINDOW; return EGL_NO_SURFACE; }
    pthread_mutex_lock(&mu);
    if (!wl_setup()) { pthread_mutex_unlock(&mu); werr = EGL_BAD_ALLOC; return EGL_NO_SURFACE; }
    struct wsurf *s = calloc(1, sizeof *s);
    s->magic = MAGIC; s->win = win; s->cfg = cfg;
    REAL(eglGetConfigAttrib)(d, cfg, EGL_NATIVE_VISUAL_ID, &s->vid);
    s->w = win->width > 0 ? win->width : 1; s->h = win->height > 0 ? win->height : 1;
    if (make_inner(d, s, s->w, s->h) < 0 || alloc_buffers(s) < 0) { free(s); pthread_mutex_unlock(&mu); werr = EGL_BAD_ALLOC; return EGL_NO_SURFACE; }
    s->next = surfs; surfs = s;
    pthread_mutex_unlock(&mu);
    LOG("window surface %p for wl_surface %p (%dx%d)\n", (void *)s, (void *)win->surface, s->w, s->h);
    return (EGLSurface)s;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c, EGLNativeWindowType w, const EGLint *a) {
    if (!wld) return REAL(eglCreateWindowSurface)(d, c, w, a);
    return create_wsurf(d, c, (void *)w);
}
EGLSurface eglCreatePlatformWindowSurfaceEXT(EGLDisplay d, EGLConfig c, void *w, const EGLint *a) {
    if (!wld) return REAL(eglCreateWindowSurface)(d, c, (EGLNativeWindowType)w, a);
    return create_wsurf(d, c, w);
}
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig c, void *w, const EGLAttrib *a) {
    (void)a;
    return eglCreatePlatformWindowSurfaceEXT(d, c, w, NULL);
}
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface sf) {
    struct wsurf *s = wfind(sf);
    if (!s) return REAL(eglDestroySurface)(d, sf);
    pthread_mutex_lock(&mu);
    struct wsurf **pp = &surfs;
    while (*pp && *pp != s) pp = &(*pp)->next;
    if (*pp) *pp = s->next;
    REAL(eglDestroySurface)(d, s->inner);
    if (s->gs) gb.sdestroy(s->gs);
    free_buffers(s);
    if (cur_draw == s) cur_draw = NULL;
    if (cur_read == s) cur_read = NULL;
    s->magic = 0; free(s);
    pthread_mutex_unlock(&mu);
    return EGL_TRUE;
}
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface sf, EGLint attr, EGLint *v) {
    struct wsurf *s = wfind(sf);
    if (s && v && (attr == EGL_WIDTH || attr == EGL_HEIGHT)) { *v = attr == EGL_WIDTH ? s->w : s->h; return EGL_TRUE; }
    return REAL(eglQuerySurface)(d, s ? s->inner : sf, attr, v);
}
EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface sf, EGLint a, EGLint v) { return REAL(eglSurfaceAttrib)(d, T(sf), a, v); }
EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface sf, EGLint b) { return REAL(eglBindTexImage)(d, T(sf), b); }
EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface sf, EGLint b) { return REAL(eglReleaseTexImage)(d, T(sf), b); }
EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface sf, EGLNativePixmapType p) { return REAL(eglCopyBuffers)(d, T(sf), p); }
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    struct wsurf *a = wfind(dr), *b = wfind(rd);
    EGLBoolean r = REAL(eglMakeCurrent)(d, a ? a->inner : dr, b ? b->inner : rd, c);
    if (r) { cur_draw = a; cur_read = b; }
    return r;
}
EGLSurface eglGetCurrentSurface(EGLint which) {
    if (which == EGL_DRAW && cur_draw) return (EGLSurface)cur_draw;
    if (which == EGL_READ && cur_read) return (EGLSurface)cur_read;
    return REAL(eglGetCurrentSurface)(which);
}
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface sf) {
    struct wsurf *s = wfind(sf);
    if (!s) return REAL(eglSwapBuffers)(d, sf);
    EGLBoolean r = REAL(eglSwapBuffers)(d, s->inner);
    if (!r) { LOG("real eglSwapBuffers failed 0x%x\n", REAL(eglGetError)()); return r; }
    if (!getenv("WLEGL_NOFINISH")) { void (*fin)(void) = R("glFinish"); if (fin) fin(); }
    pthread_mutex_lock(&mu);
    present(s);
    int nw = s->win->width, nh = s->win->height;
    if (nw > 0 && nh > 0 && (nw != s->w || nh != s->h)) resize_surf(d, s, nw, nh);
    pthread_mutex_unlock(&mu);
    return EGL_TRUE;
}
static EGLBoolean swap_damage(EGLDisplay d, EGLSurface s, const EGLint *r, EGLint n) { (void)r; (void)n; return eglSwapBuffers(d, s); }

/* ---------- plain forwarding ---------- */
#define FWD(ret, name, params, args) ret name params { __typeof__(&name) f = R(#name); return f ? f args : (ret)0; }
FWD(EGLBoolean, eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d, a, b))
FWD(EGLBoolean, eglTerminate, (EGLDisplay d), (d))
FWD(EGLBoolean, eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint n, EGLint *r), (d, c, n, r))
FWD(EGLBoolean, eglGetConfigAttrib, (EGLDisplay d, EGLConfig c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLSurface, eglCreatePbufferSurface, (EGLDisplay d, EGLConfig c, const EGLint *a), (d, c, a))
FWD(EGLSurface, eglCreatePixmapSurface, (EGLDisplay d, EGLConfig c, EGLNativePixmapType p, const EGLint *a), (d, c, p, a))
FWD(EGLBoolean, eglBindAPI, (EGLenum a), (a))
FWD(EGLenum, eglQueryAPI, (void), ())
FWD(EGLBoolean, eglWaitClient, (void), ())
FWD(EGLBoolean, eglReleaseThread, (void), ())
FWD(EGLSurface, eglCreatePbufferFromClientBuffer, (EGLDisplay d, EGLenum t, EGLClientBuffer b, EGLConfig c, const EGLint *a), (d, t, b, c, a))
FWD(EGLBoolean, eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
FWD(EGLBoolean, eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
FWD(EGLContext, eglGetCurrentContext, (void), ())
FWD(EGLDisplay, eglGetCurrentDisplay, (void), ())
FWD(EGLBoolean, eglQueryContext, (EGLDisplay d, EGLContext c, EGLint a, EGLint *v), (d, c, a, v))
FWD(EGLBoolean, eglWaitGL, (void), ())
FWD(EGLBoolean, eglWaitNative, (EGLint e), (e))

/* ---------- config selection with diagnostics + relaxation (Firefox asks for configs the blob may not list) ---------- */
static void dump_attribs(const char *tag, const EGLint *a) {
    if (!dbg || !a) return;
    fprintf(stderr, "[wlegl] %s:", tag);
    for (; *a != EGL_NONE; a += 2) fprintf(stderr, " 0x%x=0x%x", a[0], a[1]);
    fprintf(stderr, "\n");
}
static void dump_configs(EGLDisplay d) {
    static int done;
    EGLConfig cs[64]; EGLint n = 0;
    if (!dbg || done) return;
    done = 1;
    if (!REAL(eglGetConfigs)(d, cs, 64, &n)) return;
    fprintf(stderr, "[wlegl] %d configs offered (id r g b a depth stencil surface_type renderable_type visual):\n", n);
    for (int i = 0; i < n; i++) {
        EGLint v[10] = {0};
        static const EGLint at[10] = { EGL_CONFIG_ID, EGL_RED_SIZE, EGL_GREEN_SIZE, EGL_BLUE_SIZE, EGL_ALPHA_SIZE,
                                       EGL_DEPTH_SIZE, EGL_STENCIL_SIZE, EGL_SURFACE_TYPE, EGL_RENDERABLE_TYPE, EGL_NATIVE_VISUAL_ID };
        for (int k = 0; k < 10; k++) REAL(eglGetConfigAttrib)(d, cs[i], at[k], &v[k]);
        fprintf(stderr, "[wlegl]   cfg %d: %d %d %d %d %d %d %d 0x%x 0x%x 0x%x\n", i, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
    }
}
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *a, EGLConfig *c, EGLint n, EGLint *r) {
    EGLBoolean ok = REAL(eglChooseConfig)(d, a, c, n, r);
    dump_attribs("eglChooseConfig", a);
    if (ok && r && *r == 0 && a) {
        EGLint t[128]; int k = 0;
        dump_configs(d);
        for (; a[k] != EGL_NONE && k < 120; k += 2) { t[k] = a[k]; t[k + 1] = a[k + 1]; }
        t[k] = EGL_NONE;
        for (int pass = 1; pass <= 3; pass++) {
            for (int i = 0; t[i] != EGL_NONE; i += 2) {
                if (t[i] == EGL_ALPHA_SIZE) t[i + 1] = 0;
                if (pass >= 2 && t[i] == EGL_RENDERABLE_TYPE) t[i + 1] = EGL_OPENGL_ES2_BIT;
                if (pass >= 3 && (t[i] == EGL_DEPTH_SIZE || t[i] == EGL_STENCIL_SIZE)) t[i + 1] = 0;
            }
            ok = REAL(eglChooseConfig)(d, t, c, n, r);
            LOG("no config matched; relaxed pass %d -> %d configs\n", pass, r ? *r : -1);
            if (ok && r && *r > 0) break;
        }
    }
    return ok;
}
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint *a) {
    dump_attribs("eglCreateContext", a);
    EGLContext x = REAL(eglCreateContext)(d, c, s, a);
    if (x == EGL_NO_CONTEXT && a) {
        /* the blob rejects robustness/flags attributes: retry with only the client version (+minor) */
        EGLint t[12], k = 0;
        LOG("eglCreateContext failed 0x%x; retrying with version attribs only\n", REAL(eglGetError)());
        for (int i = 0; a[i] != EGL_NONE && k < 8; i += 2)
            if (a[i] == EGL_CONTEXT_CLIENT_VERSION || a[i] == 0x30fb /* EGL_CONTEXT_MINOR_VERSION */) { t[k++] = a[i]; t[k++] = a[i + 1]; }
        t[k] = EGL_NONE;
        x = REAL(eglCreateContext)(d, c, s, t);
        if (x == EGL_NO_CONTEXT) {
            LOG("retry 1 failed 0x%x; trying major version only\n", REAL(eglGetError)());
            EGLint u[3] = { EGL_CONTEXT_CLIENT_VERSION, t[0] == EGL_CONTEXT_CLIENT_VERSION ? t[1] : 2, EGL_NONE };
            x = REAL(eglCreateContext)(d, c, s, u);
            if (x == EGL_NO_CONTEXT) LOG("retry 2 failed 0x%x\n", REAL(eglGetError)());
        }
        LOG("eglCreateContext retry -> %p\n", (void *)x);
    }
    return x;
}

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name) {
    static const struct { const char *n; __eglMustCastToProperFunctionPointerType f; } t[] = {
        { "eglGetPlatformDisplayEXT", (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplayEXT },
        { "eglGetPlatformDisplay", (__eglMustCastToProperFunctionPointerType)eglGetPlatformDisplay },
        { "eglCreatePlatformWindowSurfaceEXT", (__eglMustCastToProperFunctionPointerType)eglCreatePlatformWindowSurfaceEXT },
        { "eglCreatePlatformWindowSurface", (__eglMustCastToProperFunctionPointerType)eglCreatePlatformWindowSurface },
        { "eglSwapBuffersWithDamageKHR", (__eglMustCastToProperFunctionPointerType)swap_damage },
        { "eglSwapBuffersWithDamageEXT", (__eglMustCastToProperFunctionPointerType)swap_damage },
        { "eglGetDisplay", (__eglMustCastToProperFunctionPointerType)eglGetDisplay },
        { "eglQueryString", (__eglMustCastToProperFunctionPointerType)eglQueryString },
        { "eglCreateWindowSurface", (__eglMustCastToProperFunctionPointerType)eglCreateWindowSurface },
        { "eglDestroySurface", (__eglMustCastToProperFunctionPointerType)eglDestroySurface },
        { "eglQuerySurface", (__eglMustCastToProperFunctionPointerType)eglQuerySurface },
        { "eglSurfaceAttrib", (__eglMustCastToProperFunctionPointerType)eglSurfaceAttrib },
        { "eglBindTexImage", (__eglMustCastToProperFunctionPointerType)eglBindTexImage },
        { "eglReleaseTexImage", (__eglMustCastToProperFunctionPointerType)eglReleaseTexImage },
        { "eglMakeCurrent", (__eglMustCastToProperFunctionPointerType)eglMakeCurrent },
        { "eglGetCurrentSurface", (__eglMustCastToProperFunctionPointerType)eglGetCurrentSurface },
        { "eglSwapBuffers", (__eglMustCastToProperFunctionPointerType)eglSwapBuffers },
        { "eglGetError", (__eglMustCastToProperFunctionPointerType)eglGetError },
        { "eglChooseConfig", (__eglMustCastToProperFunctionPointerType)eglChooseConfig },
        { "eglCreateContext", (__eglMustCastToProperFunctionPointerType)eglCreateContext },
    };
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(name, t[i].n)) return t[i].f;
    PFNEGLGETPROCADDRESSPROC f = (PFNEGLGETPROCADDRESSPROC)R("eglGetProcAddress");
    /* Mesa-style: core egl*() and gl*() names must resolve too (Firefox's glxtest relies on it) */
    if (self && !strncmp(name, "egl", 3)) { void *q = dlsym(self, name); if (q) return (__eglMustCastToProperFunctionPointerType)q; }
    __eglMustCastToProperFunctionPointerType p = f ? f(name) : NULL;
    if (!p && real_lib) p = (__eglMustCastToProperFunctionPointerType)dlsym(real_lib, name);
    return p;
}

__attribute__((constructor)) static void wlegl_loaded(void) {
    if (getenv("WLEGL_DEBUG")) fprintf(stderr, "[wlegl] wrapper libEGL loaded in pid %d (%s)\n", (int)getpid(), program_invocation_short_name);
}
