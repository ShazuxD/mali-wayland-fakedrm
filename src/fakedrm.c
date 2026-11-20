#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/kcmp.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <linux/fb.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

/* Fake DRM/KMS node for a kernel without CONFIG_DRM.
   - open of a /dev/dri node returns the read end of a pipe (so poll/read of flip events work)
   - GEM/dumb buffers are ION dma-bufs (what the Mali blob + kbase can import)
   - KMS: one HDMI connector, one CRTC, one primary plane; SETCRTC / PAGE_FLIP copy the
     buffer into /dev/fb0 and queue a flip-complete event, paced to FAKE_KMS_HZ (default 60).
   Env: FAKE_DRM_OK=1 (answer unknown ioctls with success)  FAKE_DRM_NAME  FAKE_ION_FLAGS
        FAKE_KMS_HZ  FAKE_KMS_LOG=1 (log every ioctl)  FAKE_FB_FILE / FAKE_FB_W / FAKE_FB_H (testing)
        FAKE_NOION=1 (use memfd instead of ION; testing only) */

#define MAXFD 1024
static unsigned char fake[MAXFD];
static int wrfd[MAXFD];
static void *rl(const char *n) { return dlsym(RTLD_NEXT, n); }
static int isdri(const char *p) { return p && !strncmp(p, "/dev/dri/", 9); }
static int mkfake(const char *p) {
    int pp[2];
    if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) return -1;
    int fd = pp[0];
    if (fd < MAXFD) fake[fd] = 1;

    /* Keep the writer independently of the read-fd number.
       SCM_RIGHTS may give the reader a different descriptor. */
    for (int i = 0; i < MAXFD; i++) {
        if (wrfd[i] == 0) {
            wrfd[i] = pp[1];
            break;
        }
    }
    fprintf(stderr, "[fakedrm] pid=%d open %s -> read=%d write=%d\n", getpid(), p, fd, pp[1]);
    return fd;
}
static mode_t getmode(int fl, va_list ap) {
    if ((fl & O_CREAT) || (fl & O_TMPFILE) == O_TMPFILE) return va_arg(ap, mode_t);
    return 0;
}
int open(const char *p, int fl, ...) {
    va_list ap; va_start(ap, fl); mode_t m = getmode(fl, ap); va_end(ap);
    if (isdri(p)) return mkfake(p);
    return ((int (*)(const char *, int, ...))rl("open"))(p, fl, m);
}
int open64(const char *p, int fl, ...) {
    va_list ap; va_start(ap, fl); mode_t m = getmode(fl, ap); va_end(ap);
    if (isdri(p)) return mkfake(p);
    return ((int (*)(const char *, int, ...))rl("open64"))(p, fl, m);
}
int openat(int dfd, const char *p, int fl, ...) {
    va_list ap; va_start(ap, fl); mode_t m = getmode(fl, ap); va_end(ap);
    if (isdri(p)) return mkfake(p);
    return ((int (*)(int, const char *, int, ...))rl("openat"))(dfd, p, fl, m);
}
int close(int fd) {
    if (fd >= 0 && fd < MAXFD && fake[fd]) {
        fake[fd] = 0;
        /* Do not close the event writer here: another process or
           descriptor may still hold the received read end. */
    }
    return ((int (*)(int))rl("close"))(fd);
}


/*
 * libseat transfers the fake DRM fd to Weston using SCM_RIGHTS.
 * The original pipe writer belongs to the opening process, so create
 * a new local pipe when the receiving process gets that descriptor.
 */
ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    ssize_t result = syscall(SYS_recvmsg, sockfd, msg, flags);

    if (result <= 0 || !msg || !msg->msg_control)
        return result;

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
         cmsg;
         cmsg = CMSG_NXTHDR(msg, cmsg)) {

        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS)
            continue;

        size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int *received = (int *)CMSG_DATA(cmsg);

        for (size_t i = 0; i < count; i++) {
            int target = received[i];
            struct stat st;

            if (syscall(SYS_fstat, target, &st) < 0 ||
                !S_ISFIFO(st.st_mode))
                continue;

            int pp[2];
            if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) {
                fprintf(stderr,
                        "[fakedrm] recvmsg: replacement pipe failed: %d\\n",
                        errno);
                continue;
            }

            if (syscall(SYS_dup3, pp[0], target, O_CLOEXEC) < 0) {
                fprintf(stderr,
                        "[fakedrm] recvmsg: dup3 failed for fd=%d: %d\\n",
                        target, errno);
                syscall(SYS_close, pp[0]);
                syscall(SYS_close, pp[1]);
                continue;
            }

            syscall(SYS_close, pp[0]);

            if (target >= 0 && target < MAXFD) {
                fake[target] = 1;
                wrfd[target] = pp[1];
            }

            fprintf(stderr,
                    "[fakedrm] pid=%d replaced received DRM pipe fd=%d writer=%d\\n",
                    getpid(), target, pp[1]);
        }
    }

    return result;
}

/* Make fake fds look like a DRM char device (226:0) to fstat/statx callers. */
static int isfake(int fd) { return fd >= 0 && fd < MAXFD && fake[fd]; }

/* libseat may pass the fake DRM pipe to Weston via SCM_RIGHTS.
   The receiving process has no entry in our process-local fake[] array. */
static int is_fake_drm_fd(int fd, unsigned long req) {
    if (isfake(fd)) return 1;

    if (_IOC_TYPE(req) == 'd') {
        struct stat st;
        if (syscall(SYS_fstat, fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
    if (fd >= 0 && fd < MAXFD)
        fake[fd] = 1;
    return 1;
}
    }
    return 0;
}
static void fixst(struct stat *s) { s->st_mode = (s->st_mode & ~S_IFMT) | S_IFCHR; s->st_rdev = makedev(226, 0); }
int fstat(int fd, struct stat *s) {
    int (*f)(int, struct stat *) = rl("fstat"); int r = f(fd, s);
    if (r == 0 && isfake(fd)) fixst(s);
    return r;
}
int fstat64(int fd, struct stat64 *s) {
    int (*f)(int, struct stat64 *) = rl("fstat64"); int r = f(fd, s);
    if (r == 0 && isfake(fd)) fixst((struct stat *)s);
    return r;
}
int __fxstat(int v, int fd, struct stat *s) {
    int (*f)(int, int, struct stat *) = rl("__fxstat"); int r;
    if (f) r = f(v, fd, s); else { int (*g)(int, struct stat *) = rl("fstat"); r = g(fd, s); }
    if (r == 0 && isfake(fd)) fixst(s);
    return r;
}
int __fxstat64(int v, int fd, struct stat64 *s) {
    int (*f)(int, int, struct stat64 *) = rl("__fxstat64"); int r;
    if (f) r = f(v, fd, s); else { int (*g)(int, struct stat64 *) = rl("fstat64"); r = g(fd, s); }
    if (r == 0 && isfake(fd)) fixst((struct stat *)s);
    return r;
}
int fstatat(int d, const char *p, struct stat *s, int fl) {
    int (*f)(int, const char *, struct stat *, int) = rl("fstatat");
    int dri = isdri(p);
    int r = f(d, dri ? "/dev/null" : p, s, fl);
    if (r == 0 && (dri || (p && !p[0] && isfake(d)))) fixst(s);
    return r;
}
int fstatat64(int d, const char *p, struct stat64 *s, int fl) {
    int (*f)(int, const char *, struct stat64 *, int) = rl("fstatat64");
    int dri = isdri(p);
    int r = f(d, dri ? "/dev/null" : p, s, fl);
    if (r == 0 && (dri || (p && !p[0] && isfake(d)))) fixst((struct stat *)s);
    return r;
}
int stat(const char *p, struct stat *s) {
    int (*f)(const char *, struct stat *) = rl("stat"); int dri = isdri(p);
    int r = f(dri ? "/dev/null" : p, s);
    if (r == 0 && dri) fixst(s);
    return r;
}
int stat64(const char *p, struct stat64 *s) {
    int (*f)(const char *, struct stat64 *) = rl("stat64"); int dri = isdri(p);
    int r = f(dri ? "/dev/null" : p, s);
    if (r == 0 && dri) fixst((struct stat *)s);
    return r;
}
int statx(int d, const char *p, int fl, unsigned m, struct statx *s) {
    int (*f)(int, const char *, int, unsigned, struct statx *) = rl("statx");
    int dri = isdri(p);
    int r = f(d, dri ? "/dev/null" : p, fl, m, s);
    if (r == 0 && (dri || (p && !p[0] && isfake(d)))) {
        s->stx_mode = (s->stx_mode & ~S_IFMT) | S_IFCHR; s->stx_rdev_major = 226; s->stx_rdev_minor = 0;
    }
    return r;
}

/* ---- fake GEM/dumb buffers backed by ION dma-bufs ---- */
#define MAXGEM 256
#define MAPOFF 0x10000000ULL
static struct {
    int used, handle_open, dmafd;
    unsigned fb_refs;
    uint64_t size;
    uint32_t pitch;
    unsigned char *map;
    size_t maplen;
} gem[MAXGEM];
struct ion_alloc_old { size_t len, align; unsigned heap_id_mask, flags; int handle; };
struct ion_fd_old { int handle; int fd; };
struct ion_alloc_new { uint64_t len; uint32_t heap_id_mask, flags, fd, unused; };
#define ION_IOC_ALLOC_OLD _IOWR('I', 0, struct ion_alloc_old)
#define ION_IOC_FREE_OLD  _IOWR('I', 1, int)
#define ION_IOC_SHARE_OLD _IOWR('I', 4, struct ion_fd_old)
#define ION_IOC_ALLOC_NEW _IOWR('I', 0, struct ion_alloc_new)
static int ion_fd = -1, ion_bit = -1, ion_new = 0;
typedef int (*ioc_t)(int, unsigned long, ...);
static int ion_alloc(size_t len) {          /* returns dma-buf fd or -1 */
    ioc_t io = rl("ioctl");
    if (getenv("FAKE_NOION")) {
        int m = syscall(SYS_memfd_create, "gem", 1 /*MFD_CLOEXEC*/);
        if (m >= 0 && ftruncate(m, len) == 0) return m;
        return -1;
    }
    if (ion_fd < 0) {
        ion_fd = ((int (*)(const char *, int, ...))rl("open"))("/dev/ion", O_RDWR | O_CLOEXEC);
        if (ion_fd < 0) { fprintf(stderr, "[fakedrm] cannot open /dev/ion\n"); return -1; }
    }
    unsigned flags = getenv("FAKE_ION_FLAGS") ? (unsigned)atoi(getenv("FAKE_ION_FLAGS")) : 1;
    for (int k = 0; k < 8; k++) {
        if (ion_bit >= 0 && k != ion_bit) continue;
        if (!ion_new) {
            struct ion_alloc_old a = { len, 4096, 1u << k, flags, 0 };
            if (io(ion_fd, ION_IOC_ALLOC_OLD, &a) == 0) {
                struct ion_fd_old s = { a.handle, -1 };
                int r = io(ion_fd, ION_IOC_SHARE_OLD, &s);
                io(ion_fd, ION_IOC_FREE_OLD, &a.handle);
                if (r == 0) {
                    if (ion_bit < 0) fprintf(stderr, "[fakedrm] ION legacy API, heap mask 0x%x works\n", 1u << k);
                    ion_bit = k; return s.fd;
                }
            } else if (errno == ENOTTY && k == 0) { ion_new = 1; }
        }
        if (ion_new) {
            struct ion_alloc_new a = { len, 1u << k, flags, 0, 0 };
            if (io(ion_fd, ION_IOC_ALLOC_NEW, &a) == 0) {
                if (ion_bit < 0) fprintf(stderr, "[fakedrm] ION new API, heap mask 0x%x works\n", 1u << k);
                ion_bit = k; return (int)a.fd;
            }
        }
    }
    fprintf(stderr, "[fakedrm] ION alloc of %zu bytes failed (errno %d)\n", len, errno);
    return -1;
}
static int gem_new(int dmafd, uint64_t size, uint32_t pitch) {
    for (int i = 1; i < MAXGEM; i++) {
        if (!gem[i].used) {
            memset(&gem[i], 0, sizeof gem[i]);
            gem[i].used = 1;
            gem[i].handle_open = 1;
            gem[i].dmafd = dmafd;
            gem[i].size = size;
            gem[i].pitch = pitch;
            return i;
        }
    }
    return -1;
}

static void log_gem_fb_refs(int h);

static void gem_maybe_release(int h) {
    if (h <= 0 || h >= MAXGEM || !gem[h].used)
        return;
    if (gem[h].handle_open || gem[h].fb_refs)
        return;
    if (gem[h].map)
        munmap(gem[h].map, gem[h].maplen);
    fprintf(stderr, "[gem-debug] RELEASE handle=%d dmafd=%d\n", h, gem[h].dmafd);
    close(gem[h].dmafd);
    memset(&gem[h], 0, sizeof gem[h]);
}

static void gem_free(int h) {
    if (h <= 0 || h >= MAXGEM || !gem[h].used || !gem[h].handle_open) {
        fprintf(stderr, "[gem-debug] CLOSE invalid/already-closed handle=%d\n", h);
        return;
    }
    fprintf(stderr, "[gem-debug] CLOSE handle=%d dmafd=%d fb_refs=%u\n",
            h, gem[h].dmafd, gem[h].fb_refs);
    log_gem_fb_refs(h);
    gem[h].handle_open = 0;
    gem_maybe_release(h);
}

static int gem_fb_get(uint32_t h) {
    if (h == 0 || h >= MAXGEM || !gem[h].used || !gem[h].handle_open)
        return 0;
    gem[h].fb_refs++;
    fprintf(stderr, "[gem-debug] FB get handle=%u refs=%u\n", h, gem[h].fb_refs);
    return 1;
}

static void gem_fb_put(uint32_t h) {
    if (h == 0 || h >= MAXGEM || !gem[h].used || !gem[h].fb_refs) {
        fprintf(stderr, "[gem-debug] FB put invalid handle=%u\n", h);
        return;
    }
    gem[h].fb_refs--;
    fprintf(stderr, "[gem-debug] FB put handle=%u refs=%u\n", h, gem[h].fb_refs);
    gem_maybe_release((int)h);
}

struct cdumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct mdumb { uint32_t handle, pad; uint64_t offset; };
struct prime { uint32_t handle, flags; int32_t fd; };
/* returns -2 if not handled, else 0 / -1 (errno set) */
static int gem_ioctl(unsigned nr, unsigned sz, void *arg) {
    if (!arg) return -2;
    if (nr == 0xb2 && sz == sizeof(struct cdumb)) {             /* MODE_CREATE_DUMB */
        struct cdumb *c = arg;
        uint32_t pitch = (c->width * ((c->bpp + 7) / 8) + 63) & ~63u;
        uint64_t size = ((uint64_t)pitch * c->height + 4095) & ~4095ULL;
        int fd = ion_alloc(size);
        if (fd < 0) { errno = ENOMEM; return -1; }
        int h = gem_new(fd, size, pitch);
        if (h < 0) { close(fd); errno = ENOMEM; return -1; }
        c->handle = h; c->pitch = pitch; c->size = size;
        fprintf(stderr, "[fakedrm]   CREATE_DUMB %ux%u bpp=%u -> handle %d pitch %u size %llu\n",
                c->width, c->height, c->bpp, h, pitch, (unsigned long long)size);
        return 0;
    }
    if (nr == 0xb3 && sz == sizeof(struct mdumb)) {             /* MODE_MAP_DUMB */
        struct mdumb *m = arg;
        if (m->handle >= MAXGEM || !gem[m->handle].used || !gem[m->handle].handle_open) { errno = ENOENT; return -1; }
        m->offset = (uint64_t)m->handle * MAPOFF;
        return 0;
    }
    if (nr == 0xb4 && sz == 4) { fprintf(stderr, "[gem-debug] DESTROY_DUMB handle=%u\\n", *(uint32_t *)arg); gem_free(*(uint32_t *)arg); return 0; }   /* MODE_DESTROY_DUMB */
    if (nr == 0x09 && sz == 8) { fprintf(stderr, "[gem-debug] GEM_CLOSE handle=%u\\n", *(uint32_t *)arg); gem_free(*(uint32_t *)arg); return 0; }   /* GEM_CLOSE */
    if (nr == 0x2d && sz == sizeof(struct prime)) {             /* PRIME_HANDLE_TO_FD */
        struct prime *p = arg;
        if (p->handle >= MAXGEM || !gem[p->handle].used || !gem[p->handle].handle_open) { errno = ENOENT; return -1; }
        int fd = fcntl(gem[p->handle].dmafd, (p->flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 3);
        if (fd < 0) return -1;
        p->fd = fd;
        struct stat st;
        if (fstat(fd, &st) == 0)
            fprintf(stderr, "[prime-debug] EXPORT handle=%u fd=%d dev=%lu ino=%lu\n",
                    p->handle, fd, (unsigned long)st.st_dev,
                    (unsigned long)st.st_ino);
        return 0;
    }
    if (nr == 0x2e && sz == sizeof(struct prime)) {             /* PRIME_FD_TO_HANDLE */
        struct prime *p = arg;
        struct stat a, b;
        for (int i = 1; i < MAXGEM; i++) {
            if (!gem[i].used || !gem[i].handle_open)
                continue;

            errno = 0;
            long same = syscall(SYS_kcmp, getpid(), getpid(),
                                KCMP_FILE, p->fd, gem[i].dmafd);
            int kerrno = errno;

            fstat(p->fd, &a);
            fstat(gem[i].dmafd, &b);

            fprintf(stderr,
                "[prime-debug] COMPARE fd=%d handle=%d dmafd=%d kcmp=%ld errno=%d dev=%lu/%lu ino=%lu/%lu\\n",
                p->fd, i, gem[i].dmafd, same, kerrno,
                (unsigned long)a.st_dev, (unsigned long)b.st_dev,
                (unsigned long)a.st_ino, (unsigned long)b.st_ino);

            if (same == 0) {
                p->handle = i;
                fprintf(stderr,
                    "[prime-debug] IMPORT SAME FILE -> handle=%d\\n", i);
                return 0;
            }
        }

        int d = fcntl(p->fd, F_DUPFD_CLOEXEC, 3);
        int h = d >= 0 ? gem_new(d, 0, 0) : -1;
        if (h < 0) { errno = ENOMEM; return -1; }
        p->handle = h;
        if (fstat(d, &a) == 0)
            fprintf(stderr, "[prime-debug] IMPORT new fd=%d -> handle=%d dev=%lu ino=%lu\n",
                    p->fd, h, (unsigned long)a.st_dev,
                    (unsigned long)a.st_ino);
        return 0;
    }
    return -2;
}
/* mmap on a fake DRM fd maps the ION dma-buf behind the handle encoded in the offset */
static void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    long r;
    if (fd >= 0 && fd < MAXFD && fake[fd]) {
        uint64_t h = (uint64_t)off / MAPOFF;
        if (h > 0 && h < MAXGEM && gem[h].used && gem[h].handle_open) {
            r = syscall(SYS_mmap, addr, len, prot, flags, gem[h].dmafd, (long)((uint64_t)off % MAPOFF));
            return r == -1 ? MAP_FAILED : (void *)r;
        }
    }
    r = syscall(SYS_mmap, addr, len, prot, flags, fd, (long)off);
    return r == -1 ? MAP_FAILED : (void *)r;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return fake_mmap(addr, len, prot, flags, fd, off); }
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) { return fake_mmap(addr, len, prot, flags, fd, off); }

/* ================= KMS emulation ================= */
#define CRTC_ID 10
#define ENC_ID 30
#define CONN_ID 20
#define PLANE_ID 40
#define FB_BASE 100
#define MAXFB 64
static struct { int used; uint32_t handle, w, h, pitch, format; } fbs[MAXFB];

static void log_gem_fb_refs(int h) {
    for (int i = 0; i < MAXFB; i++) {
        if (fbs[i].used && fbs[i].handle == (uint32_t)h) {
            fprintf(stderr,
                "[gem-debug] WARNING handle=%d still referenced by fb=%d\\n",
                h, FB_BASE + i);
        }
    }
}

static struct { uint32_t fb_id; int active; } crtc;

/* --- /dev/fb0 sink --- */
static int fb_ready;
static unsigned char *fbmem;
static uint32_t fbw = 1920, fbh = 1080, fbline = 1920 * 4;
static int fb_red16 = 1;
static void fb_init(void) {
    if (fb_ready) return;
    fb_ready = 1;
    const char *path = getenv("FAKE_FB_FILE") ? getenv("FAKE_FB_FILE") : "/dev/fb0";
    int f = ((int (*)(const char *, int, ...))rl("open"))(path, O_RDWR | O_CLOEXEC);
    struct fb_var_screeninfo vi; struct fb_fix_screeninfo fi;
    if (f >= 0 && !ioctl(f, FBIOGET_VSCREENINFO, &vi) && !ioctl(f, FBIOGET_FSCREENINFO, &fi) && vi.bits_per_pixel == 32) {
        void *m = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        if (m != MAP_FAILED) {
            fbmem = (unsigned char *)m + (size_t)vi.yoffset * fi.line_length;
            fbw = vi.xres; fbh = vi.yres; fbline = fi.line_length; fb_red16 = (vi.red.offset == 16);
        }
        fprintf(stderr, "[fakedrm] fb0 %ux%u line %u red.offset %u -> %s\n", vi.xres, vi.yres, fi.line_length, vi.red.offset, fbmem ? "ok" : "mmap failed");
    } else if (f >= 0 && getenv("FAKE_FB_W") && getenv("FAKE_FB_H")) {
        fbw = atoi(getenv("FAKE_FB_W")); fbh = atoi(getenv("FAKE_FB_H")); fbline = fbw * 4;
        void *m = mmap(NULL, (size_t)fbline * fbh, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        if (m != MAP_FAILED) fbmem = m;
        fprintf(stderr, "[fakedrm] test fb file %s %ux%u\n", path, fbw, fbh);
    } else fprintf(stderr, "[fakedrm] no usable /dev/fb0 (32 bpp needed); frames are dropped\n");
}

static void mk_mode(struct drm_mode_modeinfo *m) {
    uint32_t w = fbw, h = fbh;
    memset(m, 0, sizeof *m);
    m->hdisplay = w; m->hsync_start = w + 88; m->hsync_end = w + 88 + 44; m->htotal = w + 88 + 44 + 148;
    m->vdisplay = h; m->vsync_start = h + 4; m->vsync_end = h + 4 + 5; m->vtotal = h + 4 + 5 + 36;
    m->vrefresh = 60;
    m->clock = (uint32_t)((uint64_t)m->htotal * m->vtotal * 60 / 1000);
    m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    m->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
    snprintf(m->name, DRM_DISPLAY_MODE_LEN, "%ux%u", w, h);
}

/* --- present: dma-buf -> fb0, then pace to the refresh rate --- */
struct dsync { uint64_t flags; };
#define DMABUF_SYNC _IOW('b', 0, struct dsync)

static void dump_fb(int i, int h) {
    static int n; static uint32_t last_fb;
    if (!getenv("FAKE_DUMP") || n >= 4) return;
    unsigned char *m = gem[h].map; if (!m) return;
    fprintf(stderr, "[fakedrm] present fb%d %ux%u pitch=%u (w*4=%u) fmt=0x%08x first16:", i, fbs[i].w, fbs[i].h, fbs[i].pitch, fbs[i].w * 4, fbs[i].format);
    for (int k = 0; k < 16; k++) fprintf(stderr, " %02x", m[k]);
    fprintf(stderr, "\n");
    if (last_fb == (uint32_t)i + 1) return;      /* only when the buffer changes */
    last_fb = i + 1;
    char nm[64]; snprintf(nm, sizeof nm, "/tmp/fb_%d.ppm", n++);
    FILE *f = fopen(nm, "wb"); if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", fbs[i].w, fbs[i].h);
    for (uint32_t y = 0; y < fbs[i].h; y++) for (uint32_t x = 0; x < fbs[i].w; x++) {
        unsigned char *q = m + (size_t)y * fbs[i].pitch + x * 4; unsigned char c[3] = { q[2], q[1], q[0] };   /* XRGB little endian: B G R X */
        fwrite(c, 1, 3, f);
    }
    fclose(f); fprintf(stderr, "[fakedrm] wrote %s\n", nm);
}
static void present(uint32_t fb_id) {
    fb_init();
    if (fb_id < FB_BASE || fb_id >= FB_BASE + MAXFB || !fbs[fb_id - FB_BASE].used) return;
    int i = fb_id - FB_BASE, h = fbs[i].handle;
    if (h <= 0 || h >= MAXGEM || !gem[h].used) return;
    size_t need = (size_t)fbs[i].pitch * fbs[i].h;
    if (!gem[h].map) {
        void *m = mmap(NULL, need, PROT_READ, MAP_SHARED, gem[h].dmafd, 0);
        if (m == MAP_FAILED) { fprintf(stderr, "[fakedrm] mmap of fb %u failed (errno %d)\n", fb_id, errno); return; }
        gem[h].map = m; gem[h].maplen = need;
    }
        if (!fbmem) return;

    struct dsync s = { 1 /*READ*/ };
    int sync_start = ioctl(gem[h].dmafd, DMABUF_SYNC, &s);

    fprintf(stderr,
        "[fakedrm] DMA_BUF_SYNC START fd=%d result=%d errno=%d\n",
        gem[h].dmafd, sync_start, sync_start < 0 ? errno : 0);

    dump_fb(i, h);
    uint32_t w = fbs[i].w < fbw ? fbs[i].w : fbw, hh = fbs[i].h < fbh ? fbs[i].h : fbh;
    for (uint32_t y = 0; y < hh; y++) {
        unsigned char *src = gem[h].map + (size_t)y * fbs[i].pitch, *dst = fbmem + (size_t)y * fbline;
        if (fb_red16) memcpy(dst, src, (size_t)w * 4);
        else for (uint32_t x = 0; x < w; x++) { dst[4*x] = src[4*x+2]; dst[4*x+1] = src[4*x+1]; dst[4*x+2] = src[4*x]; dst[4*x+3] = 255; }
    }
    s.flags = 1 | 4;                            /* end */
    ioctl(gem[h].dmafd, DMABUF_SYNC, &s);
}
static void pace(void) {
    static struct timespec last;
    const char *e = getenv("FAKE_KMS_HZ");
    long hz = e ? atol(e) : 60; if (hz < 1) return;
    long per = 1000000000L / hz;
    struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
    long long dt = (n.tv_sec - last.tv_sec) * 1000000000LL + (n.tv_nsec - last.tv_nsec);
    if (last.tv_sec && dt < per) {
        struct timespec t = last; t.tv_nsec += per; while (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        last = t;
    } else last = n;
}
static int find_event_writer(int fd) {
    struct stat reader, writer;

    if (syscall(SYS_fstat, fd, &reader) < 0)
        return -1;

    fprintf(stderr, "[fakedrm] pid=%d reader fd=%d dev=%lu ino=%lu\\n",
            getpid(), fd, (unsigned long)reader.st_dev,
            (unsigned long)reader.st_ino);

    for (int i = 0; i < MAXFD; i++) {
        if (wrfd[i] <= 0)
            continue;

        if (syscall(SYS_fstat, wrfd[i], &writer) == 0) {
            fprintf(stderr, "[fakedrm] writer slot=%d fd=%d dev=%lu ino=%lu\\n",
                    i, wrfd[i], (unsigned long)writer.st_dev,
                    (unsigned long)writer.st_ino);
            if (reader.st_dev == writer.st_dev &&
                reader.st_ino == writer.st_ino)
                return wrfd[i];
        }
    }

    return -1;
}

static void send_flip_event(int fd, uint64_t user) {
    static uint32_t seq;
    struct drm_event_vblank ev;
    struct timespec n;

    clock_gettime(CLOCK_MONOTONIC, &n);
    memset(&ev, 0, sizeof ev);
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof ev;
    ev.user_data = user;
    ev.tv_sec = n.tv_sec;
    ev.tv_usec = n.tv_nsec / 1000;
    ev.sequence = ++seq;
    ev.crtc_id = CRTC_ID;

    int writer = find_event_writer(fd);
    if (writer >= 0) {
        ssize_t r = write(writer, &ev, sizeof ev);
        if (r != sizeof ev)
            fprintf(stderr, "[fakedrm] flip event write failed: fd=%d writer=%d result=%zd errno=%d\\n",
                    fd, writer, r, errno);
    } else {
        fprintf(stderr, "[fakedrm] no event writer found for fd=%d\\n", fd);
    }
}

/* --- properties --- */
struct prop { uint32_t id; const char *name; uint32_t flags; int nenum; const char *en[4]; uint64_t ev[4]; };
static const struct prop props[] = {
    { 1, "DPMS", DRM_MODE_PROP_ENUM, 4, { "On", "Standby", "Suspend", "Off" }, { 0, 1, 2, 3 } },
    { 2, "CRTC_ID", DRM_MODE_PROP_OBJECT, 0, {0}, {0} },
    { 3, "ACTIVE", DRM_MODE_PROP_RANGE, 0, {0}, {0} },
    { 4, "MODE_ID", DRM_MODE_PROP_BLOB, 0, {0}, {0} },
    { 5, "type", DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE, 3, { "Overlay", "Primary", "Cursor" }, { 0, 1, 2 } },
    { 6, "FB_ID", DRM_MODE_PROP_OBJECT, 0, {0}, {0} },
    { 7, "CRTC_ID", DRM_MODE_PROP_OBJECT, 0, {0}, {0} },
};
static uint64_t propval(uint32_t id) {
    switch (id) {
    case 1: return 0;
    case 2: return crtc.active ? CRTC_ID : 0;
    case 3: return crtc.active;
    case 4: return 0;
    case 5: return 1;
    case 6: return crtc.fb_id;
    case 7: return crtc.active ? CRTC_ID : 0;
    }
    return 0;
}
static int objprops(uint32_t type, uint32_t *ids) {
    static const uint32_t conn[] = { 1, 2 }, crt[] = { 3, 4 }, pl[] = { 5, 6, 7 };
    const uint32_t *s; int n;
    if (type == DRM_MODE_OBJECT_CONNECTOR) { s = conn; n = 2; }
    else if (type == DRM_MODE_OBJECT_CRTC) { s = crt; n = 2; }
    else if (type == DRM_MODE_OBJECT_PLANE) { s = pl; n = 3; }
    else return 0;
    memcpy(ids, s, n * 4);
    return n;
}
static void putv(uint64_t ptr, uint32_t have, const void *v, size_t item, uint32_t n) {
    if (ptr && have >= n && n) memcpy((void *)(uintptr_t)ptr, v, item * n);
}
#define ERR(e) do { errno = (e); return -1; } while (0)

static int kms_ioctl(int fd, unsigned nr, unsigned sz, void *arg) {
    (void)sz;
    if (!arg && nr != 0x1e && nr != 0x1f) return -2;
    fb_init();
    switch (nr) {
    case 0x0d: {                                    /* SET_CLIENT_CAP */
        struct drm_set_client_cap *c = arg;
        if (c->capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES) return 0;
        ERR(EINVAL);                                /* no atomic: compositors fall back to legacy KMS */
    }
    case 0xa0: {                                    /* MODE_GETRESOURCES */
        struct drm_mode_card_res *r = arg;
        uint32_t c = CRTC_ID, e = ENC_ID, n = CONN_ID;
        putv(r->crtc_id_ptr, r->count_crtcs, &c, 4, 1);
        putv(r->encoder_id_ptr, r->count_encoders, &e, 4, 1);
        putv(r->connector_id_ptr, r->count_connectors, &n, 4, 1);
        r->count_fbs = 0; r->count_crtcs = 1; r->count_encoders = 1; r->count_connectors = 1;
        r->min_width = 0; r->min_height = 0; r->max_width = 8192; r->max_height = 8192;
        return 0;
    }
    case 0xa1: {                                    /* MODE_GETCRTC */
        struct drm_mode_crtc *c = arg;
        if (c->crtc_id != CRTC_ID) ERR(ENOENT);
        c->fb_id = crtc.fb_id; c->x = c->y = 0; c->gamma_size = 0; c->mode_valid = crtc.active;
        mk_mode(&c->mode);
        return 0;
    }
    case 0xa2: {                                    /* MODE_SETCRTC */
        struct drm_mode_crtc *c = arg;
        if (c->crtc_id != CRTC_ID) ERR(ENOENT);
        if (c->fb_id && c->mode_valid) { crtc.fb_id = c->fb_id; crtc.active = 1; present(c->fb_id); }
        else if (!c->fb_id) { crtc.fb_id = 0; crtc.active = 0; }
        return 0;
    }
    case 0xa3: case 0xbb: ERR(EINVAL);              /* cursor: not offered */
    case 0xa4: case 0xa5: return 0;                 /* gamma */
    case 0xa6: {                                    /* MODE_GETENCODER */
        struct drm_mode_get_encoder *e = arg;
        if (e->encoder_id != ENC_ID) ERR(ENOENT);
        e->encoder_type = DRM_MODE_ENCODER_TMDS; e->crtc_id = crtc.active ? CRTC_ID : 0;
        e->possible_crtcs = 1; e->possible_clones = 0;
        return 0;
    }
    case 0xa7: {                                    /* MODE_GETCONNECTOR */
        struct drm_mode_get_connector *c = arg;
        struct drm_mode_modeinfo m; uint32_t enc = ENC_ID, ids[2], n = objprops(DRM_MODE_OBJECT_CONNECTOR, ids);
        uint64_t vals[2]; for (uint32_t i = 0; i < n; i++) vals[i] = propval(ids[i]);
        if (c->connector_id != CONN_ID) ERR(ENOENT);
        mk_mode(&m);
        putv(c->encoders_ptr, c->count_encoders, &enc, 4, 1);
        putv(c->modes_ptr, c->count_modes, &m, sizeof m, 1);
        putv(c->props_ptr, c->count_props, ids, 4, n);
        putv(c->prop_values_ptr, c->count_props, vals, 8, n);
        c->count_encoders = 1; c->count_modes = 1; c->count_props = n;
        c->encoder_id = ENC_ID; c->connector_type = DRM_MODE_CONNECTOR_HDMIA; c->connector_type_id = 1;
        c->connection = 1 /* connected */; c->mm_width = 520; c->mm_height = 290; c->subpixel = 1 /* subpixel unknown */;
        return 0;
    }
    case 0xaa: {                                    /* MODE_GETPROPERTY */
        struct drm_mode_get_property *p = arg;
        for (unsigned i = 0; i < sizeof props / sizeof props[0]; i++) {
            const struct prop *q = &props[i];
            if (q->id != p->prop_id) continue;
            uint32_t have_v = p->count_values, have_e = p->count_enum_blobs;
            uint64_t vals[2] = { 0, 1 }; uint32_t nv = 0;
            memset(p->name, 0, sizeof p->name); strncpy(p->name, q->name, DRM_PROP_NAME_LEN - 1);
            p->flags = q->flags;
            if (q->flags & DRM_MODE_PROP_RANGE) nv = 2;
            else if (q->flags & DRM_MODE_PROP_OBJECT) { vals[0] = DRM_MODE_OBJECT_CRTC; nv = 1; }
            putv(p->values_ptr, have_v, vals, 8, nv);
            p->count_values = nv;
            if (q->nenum && p->enum_blob_ptr && have_e >= (uint32_t)q->nenum) {
                struct drm_mode_property_enum *o = (void *)(uintptr_t)p->enum_blob_ptr;
                for (int k = 0; k < q->nenum; k++) { memset(&o[k], 0, sizeof o[k]); o[k].value = q->ev[k]; strncpy(o[k].name, q->en[k], DRM_PROP_NAME_LEN - 1); }
            }
            p->count_enum_blobs = q->nenum;
            return 0;
        }
        ERR(ENOENT);
    }
    case 0xab: case 0xba: return 0;                 /* SETPROPERTY (DPMS etc.): accept */
    case 0xac: ERR(EINVAL);                         /* GETPROPBLOB */
    case 0xad: ERR(ENOENT);                         /* GETFB */
    case 0xae: {                                    /* ADDFB */
        struct drm_mode_fb_cmd *a = arg;
        if (!gem_fb_get(a->handle)) ERR(ENOENT);
        for (int i = 0; i < MAXFB; i++) if (!fbs[i].used) {
            fbs[i].used = 1; fbs[i].handle = a->handle; fbs[i].w = a->width; fbs[i].h = a->height;
            fbs[i].pitch = a->pitch; fbs[i].format = DRM_FORMAT_XRGB8888; a->fb_id = FB_BASE + i; return 0;
        }
        gem_fb_put(a->handle);
        ERR(ENOMEM);
    }
    case 0xb8: {                                    /* ADDFB2 */
        struct drm_mode_fb_cmd2 *a = arg;
        if (!gem_fb_get(a->handles[0])) ERR(ENOENT);
        for (int i = 0; i < MAXFB; i++) if (!fbs[i].used) {
            fbs[i].used = 1; fbs[i].handle = a->handles[0]; fbs[i].w = a->width; fbs[i].h = a->height;
            fbs[i].pitch = a->pitches[0]; fbs[i].format = a->pixel_format; a->fb_id = FB_BASE + i;
            fprintf(stderr, "[fakedrm]   ADDFB2 %ux%u fmt 0x%08x handle %u pitch %u -> fb %u\n", a->width, a->height, a->pixel_format, a->handles[0], a->pitches[0], a->fb_id);
            return 0;
        }
        gem_fb_put(a->handles[0]);
        ERR(ENOMEM);
    }
    case 0xaf: {                                    /* RMFB */
        uint32_t id = *(uint32_t *)arg;
        if (id >= FB_BASE && id < FB_BASE + MAXFB) {
            int i = (int)(id - FB_BASE);
            if (fbs[i].used) {
                uint32_t h = fbs[i].handle;
                fbs[i].used = 0;
                gem_fb_put(h);
            }
        }
        if (crtc.fb_id == id) crtc.fb_id = 0;
        return 0;
    }
    case 0xb0: {                                    /* MODE_PAGE_FLIP */
        struct drm_mode_crtc_page_flip *f = arg;
        if (f->crtc_id != CRTC_ID) ERR(ENOENT);
        if (f->fb_id < FB_BASE || f->fb_id >= FB_BASE + MAXFB || !fbs[f->fb_id - FB_BASE].used) ERR(ENOENT);
        present(f->fb_id); crtc.fb_id = f->fb_id; crtc.active = 1;
        pace();
        if (f->flags & DRM_MODE_PAGE_FLIP_EVENT) send_flip_event(fd, f->user_data);
        return 0;
    }
    case 0xb1: {                                    /* DIRTYFB */
        struct drm_mode_fb_dirty_cmd *d = arg;
        present(d->fb_id); return 0;
    }
    case 0xb5: {                                    /* GETPLANERESOURCES */
        struct drm_mode_get_plane_res *r = arg; uint32_t p = PLANE_ID;
        putv(r->plane_id_ptr, r->count_planes, &p, 4, 1);
        r->count_planes = 1; return 0;
    }
    case 0xb6: {                                    /* GETPLANE */
        struct drm_mode_get_plane *p = arg;
        static const uint32_t fmts[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };
        if (p->plane_id != PLANE_ID) ERR(ENOENT);
        putv(p->format_type_ptr, p->count_format_types, fmts, 4, 2);
        p->count_format_types = 2; p->crtc_id = crtc.active ? CRTC_ID : 0; p->fb_id = crtc.fb_id;
        p->possible_crtcs = 1; p->gamma_size = 0;
        return 0;
    }
    case 0xb7: ERR(EINVAL);                         /* SETPLANE */
    case 0xb9: {                                    /* OBJ_GETPROPERTIES */
        struct drm_mode_obj_get_properties *o = arg; uint32_t ids[4]; uint64_t vals[4];
        uint32_t n = objprops(o->obj_type, ids);
        for (uint32_t i = 0; i < n; i++) vals[i] = propval(ids[i]);
        putv(o->props_ptr, o->count_props, ids, 4, n);
        putv(o->prop_values_ptr, o->count_props, vals, 8, n);
        o->count_props = n;
        return 0;
    }
    case 0xbc: ERR(EOPNOTSUPP);                     /* ATOMIC */
    case 0xbd: case 0xbe: ERR(EINVAL);              /* prop blobs */
    case 0x1e: case 0x1f: return 0;                 /* SET/DROP_MASTER */
    }
    return -2;
}

struct dv { int vmaj, vmin, vpl; size_t nlen; char *name; size_t dlen; char *date; size_t dsl; char *desc; };
static void put(size_t *len, char *buf, const char *s) {
    size_t n = strlen(s);
    if (buf && *len) memcpy(buf, s, n < *len ? n : *len);
    *len = n;
}
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (is_fake_drm_fd(fd, req)) {
        unsigned t = _IOC_TYPE(req), nr = _IOC_NR(req), sz = _IOC_SIZE(req);
        int ok = getenv("FAKE_DRM_OK") != NULL;
        if (getenv("FAKE_KMS_LOG"))
            fprintf(stderr, "[fakedrm] ioctl fd=%d type='%c' nr=0x%x size=%u\n", fd, (t >= 32 && t < 127) ? (char)t : '?', nr, sz);
        if (t == 'd' && nr == 0 && sz == sizeof(struct dv) && arg) {      /* DRM_IOCTL_VERSION */
            struct dv *v = arg;
            v->vmaj = 1; v->vmin = 0; v->vpl = 0;
            put(&v->nlen, v->name, getenv("FAKE_DRM_NAME") ? getenv("FAKE_DRM_NAME") : "rockchip");
            put(&v->dlen, v->date, "20180101");
            put(&v->dsl, v->desc, "fake drm");
            return 0;
        }
        if (t == 'd') { int gr = gem_ioctl(nr, sz, arg); if (gr != -2) return gr; }
        if (t == 'd') { int kr = kms_ioctl(fd, nr, sz, arg); if (kr != -2) return kr; }
        if (t == 'd' && nr == 0x0c && sz == 16 && arg) {                  /* DRM_IOCTL_GET_CAP */
            uint64_t *a = arg;
            if (ok) {
                const char *all = getenv("FAKE_DRM_CAPALL");
                uint64_t v = 0;
                switch (a[0]) {
                case 1: v = 1; break;               /* DUMB_BUFFER */
                case 2: v = 24; break;              /* DUMB_PREFERRED_DEPTH */
                case 5: v = 3; break;               /* PRIME import|export */
                case 6: v = 1; break;               /* TIMESTAMP_MONOTONIC */
                case 8: case 9: v = 64; break;      /* CURSOR_WIDTH/HEIGHT */
                }
                a[1] = all ? (uint64_t)atoi(all) : v;
                return 0;
            }
        }
        if (ok) return 0;
        errno = ENOTTY;
        return -1;
    }
    return ((int (*)(int, unsigned long, ...))rl("ioctl"))(fd, req, arg);
}
