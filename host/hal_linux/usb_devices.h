/*
 * usb_devices.h - list the USB devices plugged into the dock (from sysfs),
 * excluding hubs and the calculator itself.
 */
#ifndef HOST_USB_DEVICES_H
#define HOST_USB_DEVICES_H

#include <stddef.h>
#include <stdint.h>

/* sysfs_dir: normally "/sys/bus/usb/devices". Returns DOCK_OK, or DOCK_E_END
 * when index is past the last device. Index order is stable (sorted by bus
 * path). dev_class falls back to the first interface's class when the device
 * class is 0 (per-interface). */
int usb_devices_get(const char *sysfs_dir, uint8_t index, uint16_t *vid,
                    uint16_t *pid, uint8_t *dev_class, char *name, size_t cap);

#endif /* HOST_USB_DEVICES_H */
