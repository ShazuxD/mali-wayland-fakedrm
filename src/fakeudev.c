#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/sysmacros.h>

struct udev;
struct udev_enumerate;
struct udev_list_entry;
struct udev_device;

#define FAKE_SYSFS "/sys/devices/virtual/drm/card0"

struct fake_enum {
    struct udev_enumerate *real;
    char subsystem[32];
};

struct fake_entry {
    int unused;
};

struct fake_device {
    int type;
};

static struct fake_entry entry;
static struct fake_device card = { 1 };
static struct fake_device pci  = { 2 };

static int is_card(void *p) { return p == &card; }
static int is_pci(void *p)  { return p == &pci; }

/* Real libudev function lookup */
#define REAL(name) dlsym(RTLD_NEXT, #name)

/* Enumeration */
struct udev_enumerate *udev_enumerate_new(struct udev *u)
{
    struct udev_enumerate *(*fn)(struct udev *) = REAL(udev_enumerate_new);
    struct fake_enum *e;

    if (!fn)
        return NULL;

    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;

    e->real = fn(u);
    if (!e->real) {
        free(e);
        return NULL;
    }

    fprintf(stderr, "[fakeudev] enumerate_new wrapper=%p\n", (void *)e);
    return (struct udev_enumerate *)e;
}

int udev_enumerate_add_match_subsystem(struct udev_enumerate *e,
                                        const char *s)
{
    struct fake_enum *fe = (struct fake_enum *)e;

    snprintf(fe->subsystem, sizeof(fe->subsystem), "%s", s ? s : "");

    fprintf(stderr, "[fakeudev] match_subsystem: %s\n", fe->subsystem);

    if (strcmp(fe->subsystem, "drm") == 0)
        return 0;

    {
        int (*fn)(struct udev_enumerate *, const char *) =
            REAL(udev_enumerate_add_match_subsystem);
        return fn ? fn(fe->real, s) : -1;
    }
}

int udev_enumerate_add_match_sysname(struct udev_enumerate *e,
                                      const char *s)
{
    struct fake_enum *fe = (struct fake_enum *)e;

    if (strcmp(fe->subsystem, "drm") == 0) {
        fprintf(stderr, "[fakeudev] DRM match_sysname: %s\n",
                s ? s : "(null)");
        return 0;
    }

    {
        int (*fn)(struct udev_enumerate *, const char *) =
            REAL(udev_enumerate_add_match_sysname);
        return fn ? fn(fe->real, s) : -1;
    }
}

int udev_enumerate_scan_devices(struct udev_enumerate *e)
{
    struct fake_enum *fe = (struct fake_enum *)e;

    if (strcmp(fe->subsystem, "drm") == 0) {
        fprintf(stderr, "[fakeudev] DRM scan: synthetic card0\n");
        return 0;
    }

    {
        int (*fn)(struct udev_enumerate *) =
            REAL(udev_enumerate_scan_devices);
        return fn ? fn(fe->real) : -1;
    }
}

struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
    struct fake_enum *fe = (struct fake_enum *)e;

    if (strcmp(fe->subsystem, "drm") == 0)
        return (struct udev_list_entry *)&entry;

    {
        struct udev_list_entry *(*fn)(struct udev_enumerate *) =
            REAL(udev_enumerate_get_list_entry);
        return fn ? fn(fe->real) : NULL;
    }
}

struct udev_enumerate *
udev_enumerate_unref(struct udev_enumerate *e)
{
    struct fake_enum *fe = (struct fake_enum *)e;

    if (fe) {
        struct udev_enumerate *(*fn)(struct udev_enumerate *) =
            REAL(udev_enumerate_unref);

        if (fn && fe->real)
            fn(fe->real);

        free(fe);
    }

    return NULL;
}

/* List entries */
const char *udev_list_entry_get_name(struct udev_list_entry *e)
{
    if (e == (struct udev_list_entry *)&entry)
        return FAKE_SYSFS;

    {
        const char *(*fn)(struct udev_list_entry *) =
            REAL(udev_list_entry_get_name);
        return fn ? fn(e) : NULL;
    }
}

struct udev_list_entry *
udev_list_entry_get_next(struct udev_list_entry *e)
{
    if (e == (struct udev_list_entry *)&entry)
        return NULL;

    {
        struct udev_list_entry *(*fn)(struct udev_list_entry *) =
            REAL(udev_list_entry_get_next);
        return fn ? fn(e) : NULL;
    }
}

/* Device construction */
struct udev_device *
udev_device_new_from_syspath(struct udev *u, const char *path)
{
    if (path && strcmp(path, FAKE_SYSFS) == 0) {
        fprintf(stderr, "[fakeudev] synthetic DRM card0\n");
        return (struct udev_device *)&card;
    }

    {
        struct udev_device *(*fn)(struct udev *, const char *) =
            REAL(udev_device_new_from_syspath);
        return fn ? fn(u, path) : NULL;
    }
}

struct udev_device *
udev_device_new_from_subsystem_sysname(struct udev *u,
                                        const char *subsystem,
                                        const char *sysname)
{
    if (subsystem && sysname &&
        strcmp(subsystem, "drm") == 0 &&
        strcmp(sysname, "card0") == 0)
        return (struct udev_device *)&card;

    {
        struct udev_device *(*fn)(struct udev *, const char *,
                                  const char *) =
            REAL(udev_device_new_from_subsystem_sysname);
        return fn ? fn(u, subsystem, sysname) : NULL;
    }
}

const char *udev_device_get_sysname(struct udev_device *d)
{
    if (is_card(d)) return "card0";
    if (is_pci(d)) return "fake-pci";

    {
        const char *(*fn)(struct udev_device *) =
            REAL(udev_device_get_sysname);
        return fn ? fn(d) : NULL;
    }
}

const char *udev_device_get_devnode(struct udev_device *d)
{
    if (is_card(d)) return "/dev/dri/card0";

    {
        const char *(*fn)(struct udev_device *) =
            REAL(udev_device_get_devnode);
        return fn ? fn(d) : NULL;
    }
}

const char *udev_device_get_sysnum(struct udev_device *d)
{
    if (is_card(d)) return "0";

    {
        const char *(*fn)(struct udev_device *) =
            REAL(udev_device_get_sysnum);
        return fn ? fn(d) : NULL;
    }
}

dev_t udev_device_get_devnum(struct udev_device *d)
{
    if (is_card(d)) return makedev(226, 0);

    {
        dev_t (*fn)(struct udev_device *) =
            REAL(udev_device_get_devnum);
        return fn ? fn(d) : 0;
    }
}

const char *udev_device_get_syspath(struct udev_device *d)
{
    if (is_card(d)) return FAKE_SYSFS;
    if (is_pci(d)) return "/sys/devices/platform/fake-pci";

    {
        const char *(*fn)(struct udev_device *) =
            REAL(udev_device_get_syspath);
        return fn ? fn(d) : NULL;
    }
}

const char *udev_device_get_property_value(struct udev_device *d,
                                            const char *property)
{
    if (is_card(d) && property &&
        strcmp(property, "ID_SEAT") == 0)
        return "seat0";

    {
        const char *(*fn)(struct udev_device *, const char *) =
            REAL(udev_device_get_property_value);
        return fn ? fn(d, property) : NULL;
    }
}

struct udev_device *
udev_device_get_parent_with_subsystem_devtype(struct udev_device *d,
                                               const char *subsystem,
                                               const char *devtype)
{
    if (is_card(d) && subsystem &&
        strcmp(subsystem, "pci") == 0) {
        fprintf(stderr, "[fakeudev] returning synthetic PCI parent\n");
        return (struct udev_device *)&pci;
    }

    {
        struct udev_device *(*fn)(struct udev_device *,
                                  const char *, const char *) =
            REAL(udev_device_get_parent_with_subsystem_devtype);
        return fn ? fn(d, subsystem, devtype) : NULL;
    }
}

const char *udev_device_get_sysattr_value(struct udev_device *d,
                                           const char *attribute)
{
    if (is_pci(d) && attribute &&
        strcmp(attribute, "boot_vga") == 0)
        return "1";

    {
        const char *(*fn)(struct udev_device *, const char *) =
            REAL(udev_device_get_sysattr_value);
        return fn ? fn(d, attribute) : NULL;
    }
}

struct udev_device *udev_device_unref(struct udev_device *d)
{
    if (is_card(d) || is_pci(d))
        return NULL;

    {
        struct udev_device *(*fn)(struct udev_device *) =
            REAL(udev_device_unref);
        return fn ? fn(d) : NULL;
    }
}
