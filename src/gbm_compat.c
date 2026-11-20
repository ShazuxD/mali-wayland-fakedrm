#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

struct gbm_bo;
struct gbm_device;
struct gbm_surface;

static void *blob_handle;

static void *blob_sym(const char *name)
{
    if (!blob_handle) {
        blob_handle = dlopen(getenv("MALI_BLOB") ? getenv("MALI_BLOB") : "/opt/mali-wl/blob.so",
                             RTLD_NOW | RTLD_LOCAL);
        if (!blob_handle)
            fprintf(stderr, "[gbm_compat] dlopen failed: %s\n", dlerror());
    }

    void *p = blob_handle ? dlsym(blob_handle, name) : NULL;

    if (!p)
        fprintf(stderr, "[gbm_compat] missing blob symbol: %s\n", name);

    return p;
}



/* GBM FRONT BUFFER DIAGNOSTIC */
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
    static struct gbm_bo *(*lock_fn)(struct gbm_surface *);
    static uint32_t (*stride_fn)(struct gbm_bo *);
    static uint32_t (*format_fn)(struct gbm_bo *);
    static uint64_t (*modifier_fn)(struct gbm_bo *);
    static int (*fd_fn)(struct gbm_bo *);
    static void *(*map_fn)(struct gbm_bo *, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t,
                           uint32_t *, void **);
    static void (*unmap_fn)(struct gbm_bo *, void *);
    static int diagnostic_count;

    if (!lock_fn) lock_fn = blob_sym("gbm_surface_lock_front_buffer");
    if (!stride_fn) stride_fn = blob_sym("gbm_bo_get_stride");
    if (!format_fn) format_fn = blob_sym("gbm_bo_get_format");
    if (!modifier_fn) modifier_fn = blob_sym("gbm_bo_get_modifier");
    if (!fd_fn) fd_fn = blob_sym("gbm_bo_get_fd");
    if (!map_fn) map_fn = blob_sym("gbm_bo_map");
    if (!unmap_fn) unmap_fn = blob_sym("gbm_bo_unmap");

    if (!getenv("GBM_DIAG"))   /* the diagnostics below map 1920x1080; only on request */
        return lock_fn ? lock_fn(surface) : NULL;
    struct gbm_bo *bo = lock_fn ? lock_fn(surface) : NULL;
    if (!bo) {
        fprintf(stderr, "[gbm-debug] lock_front_buffer -> NULL\n");
        return NULL;
    }

    uint32_t stride = stride_fn ? stride_fn(bo) : 0;
    uint32_t format = format_fn ? format_fn(bo) : 0;
    uint64_t modifier = modifier_fn ? modifier_fn(bo) : 0;
    int fd = fd_fn ? fd_fn(bo) : -1;

    fprintf(stderr,
        "[gbm-debug] FRONT BO=%p fd=%d stride=%u format=0x%08x modifier=0x%016llx\n",
        (void *)bo, fd, stride, format,
        (unsigned long long)modifier);

    if (diagnostic_count < 10 && fd >= 0 && map_fn && unmap_fn) {
        diagnostic_count++;

        uint32_t mapped_stride = 0;
        void *map_data = NULL;

        /* GBM_BO_TRANSFER_READ = 1 */
        void *mapped = map_fn(bo, 0, 0, 1920, 1080, 1,
                              &mapped_stride, &map_data);

        fprintf(stderr,
            "[gbm-map] BO=%p mapped=%p map_stride=%u map_data=%p\n",
            (void *)bo, mapped, mapped_stride, map_data);

        if (mapped) {
            size_t raw_len = (size_t)stride * 1080;
            void *raw = mmap(NULL, raw_len, PROT_READ, MAP_SHARED, fd, 0);

            fprintf(stderr, "[gbm-map] raw_mmap=%p length=%zu\n",
                    raw, raw_len);

            if (raw != MAP_FAILED) {
                const unsigned char *a = mapped;
                const unsigned char *b = raw;

                size_t first = raw_len;
                for (size_t off = 0; off + 4 <= raw_len; off += 4) {
                    if (a[off] || a[off + 1] || a[off + 2]) {
                        first = off;
                        break;
                    }
                }

                if (first < raw_len) {
                    fprintf(stderr,
                        "[gbm-map] FIRST_NONBLACK offset=%zu pixel=%zu,%zu equal64=%d mapped:",
                        first, (first % mapped_stride) / 4,
                        first / mapped_stride,
                        memcmp(a + first, b + first, 64) == 0);

                    for (int j = 0; j < 16; j++)
                        fprintf(stderr, " %02x", a[first + j]);

                    fprintf(stderr, " raw:");
                    for (int j = 0; j < 16; j++)
                        fprintf(stderr, " %02x", b[first + j]);

                    fprintf(stderr, "\\n");
                } else {
                    fprintf(stderr, "[gbm-map] mapped BO is still black\\n");
                }
                unsigned offsets[] = {0, mapped_stride, mapped_stride * 64,
                                      mapped_stride * 540};

                for (unsigned i = 0; i < 4; i++) {
                    unsigned off = offsets[i];
                    if (off + 64 > raw_len ||
                        off + 64 > (size_t)mapped_stride * 1080)
                        continue;

                    fprintf(stderr,
                        "[gbm-map] offset=%u equal64=%d mapped:",
                        off, memcmp(a + off, b + off, 64) == 0);

                    for (int j = 0; j < 16; j++)
                        fprintf(stderr, " %02x", a[off + j]);

                    fprintf(stderr, " raw:");
                    for (int j = 0; j < 16; j++)
                        fprintf(stderr, " %02x", b[off + j]);

                    fprintf(stderr, "\n");
                }

                munmap(raw, raw_len);
            } else {
                fprintf(stderr, "[gbm-map] raw mmap failed errno=%d\n", errno);
            }

            unmap_fn(bo, map_data);
        }
    }

    /* gbm_bo_get_fd() returns a caller-owned duplicate. */
    if (fd >= 0)
        close(fd);

    return bo;
}

int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
    static int (*real_fn)(struct gbm_bo *, int);
    if (!real_fn)
        real_fn = blob_sym("gbm_bo_get_fd_per_plane");
    if (real_fn)
        return real_fn(bo, plane);
    {   /* blob has no per-plane export: plane 0 is the single fd */
        int (*g)(struct gbm_bo *) = blob_sym("gbm_bo_get_fd");
        return (plane == 0 && g) ? g(bo) : -1;
    }
}

struct gbm_bo *gbm_bo_create_with_modifiers2(
    struct gbm_device *gbm, uint32_t width, uint32_t height,
    uint32_t format, const uint64_t *modifiers,
    const unsigned int count, uint32_t flags)
{
    static struct gbm_bo *(*real_fn)(
        struct gbm_device *, uint32_t, uint32_t, uint32_t,
        const uint64_t *, unsigned int);

    if (!real_fn)
        real_fn = blob_sym("gbm_bo_create_with_modifiers");

    fprintf(stderr,
        "[gbm_compat] modifiers2 %ux%u format=0x%x count=%u flags=0x%x\n",
        width, height, format, count, flags);

    return real_fn
        ? real_fn(gbm, width, height, format, modifiers, count)
        : NULL;
}

struct gbm_surface *gbm_surface_create(
    struct gbm_device *gbm, uint32_t width, uint32_t height,
    uint32_t format, uint32_t flags)
{
    static struct gbm_surface *(*real_fn)(
        struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);

    if (!real_fn)
        real_fn = blob_sym("gbm_surface_create");

    fprintf(stderr,
        "[gbm_compat] surface_create %ux%u format=0x%x flags=0x%x\n",
        width, height, format, flags);

    uint32_t compatible_flags = flags & ~0x10u;

    if (flags != compatible_flags)
        fprintf(stderr,
            "[gbm_compat] removing unsupported GBM_BO_USE_LINEAR: 0x%x -> 0x%x\\n",
            flags, compatible_flags);

    struct gbm_surface *result = real_fn
        ? real_fn(gbm, width, height, format, compatible_flags)
        : NULL;

    fprintf(stderr, "[gbm_compat] surface_create -> %p\n", (void *)result);
    return result;
}

struct gbm_bo *gbm_bo_create(
    struct gbm_device *gbm, uint32_t width, uint32_t height,
    uint32_t format, uint32_t flags)
{
    static struct gbm_bo *(*real_fn)(
        struct gbm_device *, uint32_t, uint32_t, uint32_t, uint32_t);

    if (!real_fn)
        real_fn = blob_sym("gbm_bo_create");

    fprintf(stderr,
        "[gbm_compat] bo_create %ux%u format=0x%x flags=0x%x\n",
        width, height, format, flags);

    struct gbm_bo *result = real_fn
        ? real_fn(gbm, width, height, format, flags)
        : NULL;

    fprintf(stderr, "[gbm_compat] bo_create -> %p\n", (void *)result);
    return result;
}

struct gbm_surface *gbm_surface_create_with_modifiers(
    struct gbm_device *gbm, uint32_t width, uint32_t height,
    uint32_t format, const uint64_t *modifiers, unsigned int count)
{
    static struct gbm_surface *(*real_fn)(
        struct gbm_device *, uint32_t, uint32_t, uint32_t,
        const uint64_t *, unsigned int);

    if (!real_fn)
        real_fn = blob_sym("gbm_surface_create_with_modifiers");

    fprintf(stderr,
        "[gbm_compat] surface_modifiers %ux%u format=0x%x count=%u\n",
        width, height, format, count);

    struct gbm_surface *result = real_fn
        ? real_fn(gbm, width, height, format, modifiers, count)
        : NULL;

    fprintf(stderr,
        "[gbm_compat] surface_modifiers -> %p\n", (void *)result);
    return result;
}
