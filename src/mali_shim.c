#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

/* KBASE_IOCTL_VERSION_CHECK payload */
struct vc { uint16_t major, minor; };
/* KBASE_IOCTL_JOB_SUBMIT payload */
struct js { uint64_t addr; uint32_t nr_atoms; uint32_t stride; };

/* Logging: quiet by default (failures only, capped); KBASE_DEBUG=1 logs everything. */
static int fail_cnt;
static int debug(void) {
    static int d = -1;
    if (d < 0) d = getenv("KBASE_DEBUG") != NULL;
    return d;
}
static int may_log_fail(int e) {
    if (e == EINTR || e == EAGAIN) return 0;
    if (debug()) return 1;
    return fail_cnt++ < 100;
}

int ioctl(int fd, unsigned long req, ...) {
    static int (*real)(int, unsigned long, ...);
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    /* 1) version handshake: optionally override sent version / fake the reply */
    if (_IOC_TYPE(req) == 0x80 && _IOC_NR(req) == 0 && arg &&
        _IOC_SIZE(req) == sizeof(struct vc)) {
        struct vc *v = arg, orig = *v;
        const char *m = getenv("KBASE_MAJOR"), *n = getenv("KBASE_MINOR");
        if (m && n) { v->major = atoi(m); v->minor = atoi(n); }
        int r = real(fd, req, arg);
        int e = errno;
        if (debug() || r < 0)
            fprintf(stderr, "[shim] version_check: blob wants %u.%u, sent %s, ret=%d errno=%d, kernel replied %u.%u\n",
                    orig.major, orig.minor, (m && n) ? "override" : "as-is", r, r < 0 ? e : 0,
                    v->major, v->minor);
        if (getenv("KBASE_SPOOF")) { *v = orig; errno = 0; return 0; }
        errno = e;
        return r;
    }

    /* 2) job submit: newer blobs send 64-byte base_jd_atom (8-byte seq_nr first);
          old kernels only take base_jd_atom_v2 (no seq_nr). Repack 64 -> kernel stride. */
    if (_IOC_TYPE(req) == 0x80 && _IOC_NR(req) == 2 && arg &&
        _IOC_SIZE(req) == sizeof(struct js)) {
        struct js *in = arg;
        if (debug())
            fprintf(stderr, "[shim] job_submit: size=%u nr_atoms=%u stride=%u addr=%p\n",
                    (unsigned)_IOC_SIZE(req), in->nr_atoms, in->stride, (void *)(uintptr_t)in->addr);
        const char *ks = getenv("KBASE_ATOM_STRIDE");
        uint32_t ksz = ks ? (uint32_t)atoi(ks) : 56;
        if (ksz > 56) ksz = 56;
        if (in->stride == 64 && ksz < 64 && in->nr_atoms && in->addr) {
            uint32_t n = in->nr_atoms;
            uint8_t *nb = malloc((size_t)n * ksz);
            if (!nb) { errno = ENOMEM; return -1; }
            for (uint32_t i = 0; i < n; i++)
                memcpy(nb + (size_t)i * ksz,
                       (uint8_t *)(uintptr_t)in->addr + (size_t)i * 64 + 8, ksz);
            struct js out = { (uint64_t)(uintptr_t)nb, n, ksz };
            int r = real(fd, req, &out);
            int e = errno;
            free(nb);
            if (debug() || (r < 0 && may_log_fail(e)))
                fprintf(stderr, "[shim] job_submit repacked %u atoms 64->%u ret=%d errno=%d\n",
                        n, ksz, r, r < 0 ? e : 0);
            errno = e;
            return r;
        }
    }

    int r = real(fd, req, arg);
    int e = errno;
    if (_IOC_TYPE(req) == 0x80 && (debug() || (r < 0 && may_log_fail(e))))
        fprintf(stderr, "[shim] kbase ioctl nr=%lu size=%lu ret=%d errno=%d\n",
                (unsigned long)_IOC_NR(req), (unsigned long)_IOC_SIZE(req), r, r < 0 ? e : 0);
    errno = e;
    return r;
}
