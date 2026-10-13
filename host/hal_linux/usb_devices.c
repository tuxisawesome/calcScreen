/* usb_devices.c - see usb_devices.h. */
#define _GNU_SOURCE
#include "usb_devices.h"
#include "dock_common.h"
#include "dusb.h"
#include "util.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LISTED 64

typedef struct usbdev {
    char node[64]; /* e.g. "1-1.2" */
} usbdev_t;

static bool read_hex(const char *dir, const char *node, const char *attr,
                     unsigned long *out)
{
    char path[PATH_MAX], buf[32], *end;
    snprintf(path, sizeof path, "%s/%s/%s", dir, node, attr);
    if (!read_text_file(path, buf, sizeof buf) || !buf[0])
        return false;
    *out = strtoul(buf, &end, 16);
    return *end == '\0';
}

/* Device nodes look like "1-1" or "1-1.4"; "usb1" are root hubs and
 * "1-1:1.0" are interfaces. */
static bool is_device_node(const char *n)
{
    return n[0] >= '0' && n[0] <= '9' && strchr(n, '-') && !strchr(n, ':');
}

static bool is_calculator(unsigned long vid, unsigned long pid)
{
    return (vid == DOCK_USB_VID && pid == DOCK_USB_PID) ||
           (vid == DOCK_DUSB_VID && pid == DOCK_DUSB_PID_84PCE);
}

static int cmp_dev(const void *a, const void *b)
{
    return strcmp(((const usbdev_t *)a)->node, ((const usbdev_t *)b)->node);
}

static int list_devices(const char *dir, usbdev_t *out, int max)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        unsigned long cls = 0, vid = 0, pid = 0;
        if (!is_device_node(e->d_name) || strlen(e->d_name) >= sizeof out[n].node)
            continue;
        if (!read_hex(dir, e->d_name, "idVendor", &vid) ||
            !read_hex(dir, e->d_name, "idProduct", &pid))
            continue;
        if (read_hex(dir, e->d_name, "bDeviceClass", &cls) && cls == 0x09)
            continue; /* hub */
        if (is_calculator(vid, pid))
            continue;
        str_copy(out[n++].node, sizeof out[0].node, e->d_name);
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], cmp_dev);
    return n;
}

int usb_devices_get(const char *sysfs_dir, uint8_t index, uint16_t *vid,
                    uint16_t *pid, uint8_t *dev_class, char *name, size_t cap)
{
    usbdev_t devs[MAX_LISTED];
    unsigned long v = 0, p = 0, cls = 0;
    char path[PATH_MAX], iface[96];
    int n = list_devices(sysfs_dir, devs, MAX_LISTED);
    const char *node;

    if (index >= n)
        return DOCK_E_END;
    node = devs[index].node;
    read_hex(sysfs_dir, node, "idVendor", &v);
    read_hex(sysfs_dir, node, "idProduct", &p);
    if (!read_hex(sysfs_dir, node, "bDeviceClass", &cls) || cls == 0) {
        snprintf(iface, sizeof iface, "%s:1.0", node);
        cls = 0;
        read_hex(sysfs_dir, iface, "bInterfaceClass", &cls);
    }
    *vid = (uint16_t)v;
    *pid = (uint16_t)p;
    *dev_class = (uint8_t)cls;

    snprintf(path, sizeof path, "%s/%s/product", sysfs_dir, node);
    if (!read_text_file(path, name, cap) || !name[0]) {
        snprintf(path, sizeof path, "%s/%s/manufacturer", sysfs_dir, node);
        if (!read_text_file(path, name, cap) || !name[0])
            str_copy(name, cap, "USB device");
    }
    /* Keep the reply printable ASCII. */
    for (char *c = name; *c; c++)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E)
            *c = '?';
    return DOCK_OK;
}
