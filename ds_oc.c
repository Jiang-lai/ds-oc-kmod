#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/hid.h> // For USB_CLASS_HID

// Define a structure to hold original endpoint intervals
struct ep_restore_info {
    __u8 ep_address;            // Endpoint address
    unsigned short original_interval; // Original bInterval value
};

#define MAX_PATCHED_EPS 2 // Max endpoints we'll try to patch (e.g., 1 IN, 1 OUT on HID Interface)
static struct ep_restore_info restore_infos[MAX_PATCHED_EPS];
static int num_actually_patched_eps = 0; // Number of EPs whose original intervals are currently stored in restore_infos

#define DS_VID 0x054c // Sony Vendor ID
#define DS_PID 0x0ce6 // DualSense Product ID

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Original Author, You & Gemini"); // Acknowledge original and modifications
MODULE_DESCRIPTION("Kernel module to set the polling rate of Sony DualSense controller on XHCI.");
MODULE_VERSION("1.1-DS"); // Version indicating DualSense support

static struct usb_device* dualsense_device = NULL; // Pointer to the managed DualSense device
static unsigned short configured_interval = 1;     // Target bInterval value (module parameter)

// Patches or restores bInterval for relevant endpoints on the DualSense controller
static void patch_or_restore_dualsense_endpoints(bool restore) {
    if (dualsense_device == NULL || dualsense_device->actconfig == NULL) {
        printk(KERN_INFO "ds_oc: DualSense device or active config not found.\n");
        return;
    }

    bool changed_anything = false;

    // When patching (not restoring), reset the count of patched EPs.
    // We will re-populate restore_infos with current values before changing them.
    if (!restore) {
        num_actually_patched_eps = 0;
        // No need to memset restore_infos here, we'll overwrite entries.
    }

    // Iterate through all interfaces in the active configuration
    for (int i = 0; i < dualsense_device->actconfig->desc.bNumInterfaces; ++i) {
        struct usb_interface* interface = dualsense_device->actconfig->interface[i];
        // Ensure interface and its current alternate setting are valid
        if (interface == NULL || interface->cur_altsetting == NULL) {
            continue;
        }

        // Check if this is the HID interface (bInterfaceClass 3)
        if (interface->cur_altsetting->desc.bInterfaceClass == USB_CLASS_HID) {
            printk(KERN_INFO "ds_oc: Found HID interface #%d (bInterfaceNumber %u).\n",
                   i, interface->cur_altsetting->desc.bInterfaceNumber);
            struct usb_host_interface* altsettingptr = interface->cur_altsetting;

            // Iterate through endpoints of this HID interface's current altsetting
            for (__u8 ep_idx = 0; ep_idx < altsettingptr->desc.bNumEndpoints; ++ep_idx) {
                struct usb_host_endpoint* ep = &altsettingptr->endpoint[ep_idx];
                struct usb_endpoint_descriptor* ep_desc = &ep->desc;

                // We are interested in Interrupt IN and Interrupt OUT endpoints
                if (usb_endpoint_is_int_in(ep_desc) || usb_endpoint_is_int_out(ep_desc)) {
                    unsigned short target_binterval; // The bInterval value we want to set

                    if (restore) {
                        // Restore operation: find this endpoint in our saved restore_infos
                        bool found_restore_info = false;
                        for (int k = 0; k < num_actually_patched_eps; ++k) {
                            if (restore_infos[k].ep_address == ep_desc->bEndpointAddress) {
                                target_binterval = restore_infos[k].original_interval;
                                found_restore_info = true;
                                printk(KERN_INFO "ds_oc: Restoring EP 0x%.2x to bInterval %u.\n",
                                       ep_desc->bEndpointAddress, target_binterval);
                                break;
                            }
                        }
                        if (!found_restore_info) {
                            // If no restore data for this EP, skip it
                            printk(KERN_WARNING "ds_oc: No restore info for EP 0x%.2x, skipping restore for this EP.\n", ep_desc->bEndpointAddress);
                            continue;
                        }
                    } else {
                        // Patch operation: save original interval if not already done and space permits
                        if (num_actually_patched_eps < MAX_PATCHED_EPS) {
                            restore_infos[num_actually_patched_eps].ep_address = ep_desc->bEndpointAddress;
                            restore_infos[num_actually_patched_eps].original_interval = ep_desc->bInterval;
                            // Actual increment of num_actually_patched_eps happens after successful change
                        } else {
                            printk(KERN_WARNING "ds_oc: Reached MAX_PATCHED_EPS (%d), cannot save original interval for EP 0x%.2x. Skipping patch for this EP.\n",
                                   MAX_PATCHED_EPS, ep_desc->bEndpointAddress);
                            continue; // Skip patching if we can't store its original state
                        }
                        target_binterval = configured_interval; // Use the module parameter value
                        printk(KERN_INFO "ds_oc: Preparing to patch EP 0x%.2x from bInterval %u to %u.\n",
                               ep_desc->bEndpointAddress, ep_desc->bInterval, target_binterval);
                    }

                    // If the current bInterval is different from our target, change it
                    if (ep_desc->bInterval != target_binterval) {
                        ep_desc->bInterval = target_binterval;
                        changed_anything = true;
                        if (!restore) {
                            // If patching, and we are here, it means we successfully stored restore_info
                            // and it was within MAX_PATCHED_EPS. So, increment the count of managed EPs.
                            num_actually_patched_eps++;
                        }
                        printk(KERN_INFO "ds_oc: EP 0x%.2x bInterval set to %u in memory.\n", ep_desc->bEndpointAddress, target_binterval);
                    }
                }
            }
            // Assuming the DualSense has only one HID interface relevant for this.
            // If other interfaces had HID EPs of interest, remove this break.
            break;
        }
    }

    if (changed_anything) {
        printk(KERN_INFO "ds_oc: Attempting to apply bInterval changes by resetting the USB device.\n");
        int ret_lock = usb_lock_device_for_reset(dualsense_device, NULL);
        if (ret_lock) {
            printk(KERN_ERR "ds_oc: Failed to acquire lock for USB device (error: %d). Resetting anyway...\n", ret_lock);
        }

        int ret_reset = usb_reset_device(dualsense_device);
        if (ret_reset) {
            printk(KERN_ERR "ds_oc: Could not reset USB device (error: %d). bInterval changes may not be effective.\n", ret_reset);
        } else {
            printk(KERN_INFO "ds_oc: USB device reset successfully. New bInterval values should be active.\n");
        }

        if (!ret_lock) { // Only unlock if lock succeeded
            usb_unlock_device(dualsense_device);
        }
    } else {
        printk(KERN_INFO "ds_oc: No bInterval changes made to DualSense endpoints or no relevant EPs found.\n");
    }
}

// Notifier callback for USB device add/remove events
static int on_usb_notify(struct notifier_block* self, unsigned long action, void* udev) {
    struct usb_device* device = udev;

    // Filter for the DualSense controller
    if (device->descriptor.idVendor != DS_VID || device->descriptor.idProduct != DS_PID) {
        return NOTIFY_OK;
    }

    switch (action) {
        case USB_DEVICE_ADD:
            if (dualsense_device == NULL) { // Manage if no device is currently managed
                dualsense_device = usb_get_dev(device); // Increment device reference count
                printk(KERN_INFO "ds_oc: DualSense controller connected (VID:0x%04x, PID:0x%04x).\n",
                       device->descriptor.idVendor, device->descriptor.idProduct);
                patch_or_restore_dualsense_endpoints(false); // 'false' to patch
            } else {
                printk(KERN_INFO "ds_oc: Another DualSense controller connected, but one (VID:0x%04x, PID:0x%04x) is already being managed.\n",
                dualsense_device->descriptor.idVendor, dualsense_device->descriptor.idProduct);
            }
            break;

        case USB_DEVICE_REMOVE:
            if (dualsense_device == device) {
                printk(KERN_INFO "ds_oc: Managed DualSense controller disconnected.\n");
                // Restore is handled by module exit if module is removed.
                // If device is removed, its state is gone. Release our reference.
                usb_put_dev(dualsense_device);
                dualsense_device = NULL;
                num_actually_patched_eps = 0; // Reset patch count
            }
            break;
    }
    return NOTIFY_OK;
}

static struct notifier_block usb_nb = {
    .notifier_call = on_usb_notify,
};

// Callback to check for devices already present when the module loads
static int check_existing_device_cb(struct usb_device* device, void* data_unused) {
    if (device->descriptor.idVendor == DS_VID && device->descriptor.idProduct == DS_PID) {
        if (dualsense_device == NULL) { // Manage if no device is currently managed
            dualsense_device = usb_get_dev(device); // Increment refcount
            printk(KERN_INFO "ds_oc: DualSense controller found at module load (VID:0x%04x, PID:0x%04x).\n",
                   device->descriptor.idVendor, device->descriptor.idProduct);
            patch_or_restore_dualsense_endpoints(false); // 'false' to patch
            return 1; // Found and processed, stop iterating
        } else {
             printk(KERN_INFO "ds_oc: DualSense controller found at module load, but one is already managed. Skipping.\n");
             return 1; // Stop if already handled
        }
    }
    return 0; // Continue iterating
}

// Module initialization function
static int __init ds_oc_init(void) {
    // Validate and clamp configured_interval (rate)
    if (configured_interval == 0) {
        printk(KERN_WARNING "ds_oc: 'rate' parameter is 0, defaulting to 1.\n");
        configured_interval = 1;
    } else if (configured_interval > 255) {
        printk(KERN_WARNING "ds_oc: 'rate' parameter (%u) > 255, clamping to 255.\n", configured_interval);
        configured_interval = 255;
    }
    // Note: For High-Speed USB devices, bInterval typically maxes out at 16.
    // If DualSense operates in High-Speed for these endpoints, values > 16 might not be effective or valid.
    // The USB spec for HS interrupt EPs: value from 1 to 16. (1=125us, 2=250us, ..., 16=32ms)
    // For FS interrupt EPs: value from 1 to 255. (1=1ms, ..., 255=255ms)
    // The DualSense lsusb output shows bcdUSB 2.00, it could be HS or FS for the HID endpoints.
    // Default bInterval 6 (for HID EPs) suggests 6ms (FS) or 4ms (HS, 2^(6-1)*125us).
    // If setting to 1 for HS, it targets 8000Hz. For FS, 1000Hz. Both are common "overclock" targets.

    printk(KERN_INFO "ds_oc: Module loaded. Configured interval (rate): %u\n", configured_interval);

    // Check for already connected DualSense devices
    usb_for_each_dev(NULL, check_existing_device_cb);

    // Register USB notifier
    usb_register_notify(&usb_nb);

    return 0;
}

// Module exit function
static void __exit ds_oc_exit(void) {
    // Unregister USB notifier first
    usb_unregister_notify(&usb_nb);

    if (dualsense_device != NULL) {
        printk(KERN_INFO "ds_oc: Module unloading. Restoring original bInterval for DualSense controller.\n");
        patch_or_restore_dualsense_endpoints(true); // 'true' to restore
        usb_put_dev(dualsense_device); // Release our reference to the device
        dualsense_device = NULL;
    }
    printk(KERN_INFO "ds_oc: Module unloaded.\n");
}

module_init(ds_oc_init);
module_exit(ds_oc_exit);

// Callback for when the 'rate' module parameter is changed at runtime
static int on_interval_changed(const char* value, const struct kernel_param* kp) {
    int ret = param_set_ushort(value, kp); // This updates configured_interval
    if (ret == 0) {
        // Validate and clamp again if changed at runtime
        if (configured_interval == 0) {
            printk(KERN_WARNING "ds_oc: 'rate' (runtime) is 0, defaulting to 1.\n");
            configured_interval = 1;
        } else if (configured_interval > 255) {
            printk(KERN_WARNING "ds_oc: 'rate' (runtime %u) > 255, clamping to 255.\n", configured_interval);
            configured_interval = 255;
        }
        printk(KERN_INFO "ds_oc: 'rate' changed to %u.\n", configured_interval);

        if (dualsense_device != NULL) {
            // Re-apply the patching logic with the new interval.
            // The patch function will save current (original) values again before applying the new one.
            patch_or_restore_dualsense_endpoints(false); // 'false' to patch with new rate
        } else {
            printk(KERN_INFO "ds_oc: 'rate' changed, but no DualSense controller is currently managed.\n");
        }
    }
    return ret;
}

static const struct kernel_param_ops interval_ops = {
    .set = on_interval_changed,
    .get = param_get_ushort, // Standard getter for unsigned short
};

// Define the 'rate' module parameter
module_param_cb(rate, &interval_ops, &configured_interval, 0644);
MODULE_PARM_DESC(rate, "Desired bInterval for DualSense HID endpoints (1-255). Default: 1. Lower is faster polling. (e.g., 1 attempts 1000Hz/8000Hz).");
