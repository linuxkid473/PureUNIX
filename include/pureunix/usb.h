#ifndef PUREUNIX_USB_H
#define PUREUNIX_USB_H

#include <pureunix/stdio.h>
#include <pureunix/types.h>

/* Per-transfer/per-report/per-poll USB stack logging: command completions,
 * transfer events, armed-transfer/doorbell notices, raw HID reports, and
 * per-port poll spam. This was added wholesale while chasing the real-
 * hardware xHCI keyboard bring-up bugs (see kernel/vmm.c's
 * vmm_map_mmio_uc()/vmm_map_framebuffer_wc() comments) and fires on every
 * interrupt completion and every key press once a keyboard is attached --
 * useful while debugging that class of bug, unusable as normal boot output.
 * Off by default; flip to 1 (or -DUSB_DEBUG=1) to get it back on serial.
 * One-shot boot messages (controller discovery, BIOS handoff, reset, port
 * enumeration, keyboard-attached, and all error paths) stay as plain
 * printf() and are unaffected by this flag. */
#ifndef USB_DEBUG
#define USB_DEBUG 0
#endif

#if USB_DEBUG
#define usb_debugf(...) printf(__VA_ARGS__)
#else
/* Not a bare ((void)0): that would leave every argument otherwise-unused
 * when USB_DEBUG is off, e.g. arm_interrupt_transfer()'s trb_phys in
 * drivers/xhci.c, which exists only to be logged here. The dead `if (0)`
 * branch still references each argument (silencing -Wunused-variable) but
 * is compiled out entirely at -O2, so printf is never actually called. */
#define usb_debugf(...) do { if (0) { printf(__VA_ARGS__); } } while (0)
#endif

/* Host-controller-agnostic USB core: standard descriptor layouts (USB 2.0
 * spec chapter 9) and the generic device-enumeration sequence, driven
 * through a small host-controller-interface vtable (usb_hc_ops_t) rather
 * than calling into drivers/xhci.c directly. xhci.c is the only
 * implementation today, but this is the seam the task's own USB-stack
 * requirements call out explicitly (reusable for a future host controller
 * or, more immediately, so drivers/hid.c doesn't need to know anything
 * xHCI-specific to submit a control transfer). See docs/usb.md. */

/* ---- Standard descriptor types (bDescriptorType), USB 2.0 spec sec 9.4 -- */
#define USB_DESC_TYPE_DEVICE         1U
#define USB_DESC_TYPE_CONFIGURATION  2U
#define USB_DESC_TYPE_STRING         3U
#define USB_DESC_TYPE_INTERFACE      4U
#define USB_DESC_TYPE_ENDPOINT       5U

/* ---- Standard request codes (bRequest) and request-type bits
 * (bmRequestType), USB 2.0 spec sec 9.4 --------------------------------- */
#define USB_REQ_GET_DESCRIPTOR    6U
#define USB_REQ_SET_CONFIGURATION 9U

#define USB_REQUEST_TYPE_DEVICE_TO_HOST 0x80U
#define USB_REQUEST_TYPE_HOST_TO_DEVICE 0x00U
#define USB_REQUEST_TYPE_STANDARD       0x00U
#define USB_REQUEST_TYPE_CLASS          0x20U
#define USB_REQUEST_TYPE_RECIPIENT_DEVICE    0x00U
#define USB_REQUEST_TYPE_RECIPIENT_INTERFACE 0x01U

/* String descriptor language ID: US English -- the only one this driver
 * ever requests (manufacturer/product strings, when present). */
#define USB_LANGID_US_ENGLISH 0x0409U

typedef struct __attribute__((packed)) usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t; /* 18 bytes */

typedef struct __attribute__((packed)) usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength; /* size of this descriptor + all interface/endpoint descriptors that follow it */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t; /* 9 bytes */

typedef struct __attribute__((packed)) usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_descriptor_t; /* 9 bytes */

typedef struct __attribute__((packed)) usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress; /* bit7 = direction (1=IN), bits3:0 = endpoint number */
    uint8_t  bmAttributes;     /* bits1:0 = transfer type (3 = Interrupt) */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t; /* 7 bytes */

#define USB_ENDPOINT_ADDRESS_NUMBER(addr) ((uint8_t)(addr) & 0x0FU)
#define USB_ENDPOINT_ADDRESS_IS_IN(addr)  (((uint8_t)(addr) & 0x80U) != 0)
#define USB_ENDPOINT_ATTR_TYPE(attr)      ((uint8_t)(attr) & 0x03U)
#define USB_ENDPOINT_TYPE_INTERRUPT 3U

typedef struct __attribute__((packed)) usb_string_descriptor_header {
    uint8_t bLength; /* total descriptor length, including this header */
    uint8_t bDescriptorType;
    /* followed by (bLength-2) bytes of UTF-16LE code units, unterminated */
} usb_string_descriptor_header_t;

/* Upper bound on a configuration descriptor's total size (config + every
 * interface/endpoint descriptor that follows it) this driver will parse --
 * a Boot Protocol HID keyboard's is typically well under 64 bytes; 256
 * leaves generous headroom without needing a dynamic allocation. Larger
 * configurations are truncated (logged) rather than rejected outright. */
#define USB_MAX_CONFIG_DESC_SIZE 256U

/* Upper bound on a decoded (UTF-16LE -> ASCII) manufacturer/product string,
 * including the terminating NUL. */
#define USB_MAX_STRING_LEN 64U

/* Everything this driver learned about one enumerated device: identity
 * (VID/PID/class), human-readable strings when present, and -- the only
 * endpoint type this driver's HID boot-keyboard support (drivers/hid.c)
 * needs -- the first Interrupt IN endpoint found across the selected
 * configuration's interfaces, if any. Populated by usb_enumerate_port(). */
typedef struct usb_device {
    uint32_t slot_id;
    uint16_t vid;
    uint16_t pid;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    char manufacturer[USB_MAX_STRING_LEN];
    char product[USB_MAX_STRING_LEN];

    bool    has_interrupt_in_endpoint;
    uint8_t interface_number;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t endpoint_address;
    uint16_t endpoint_max_packet_size;
    uint8_t endpoint_interval;
} usb_device_t;

/* Host-controller-interface vtable: everything the generic enumeration
 * sequence (usb_enumerate_port()) and class drivers (drivers/hid.c) need
 * from the underlying host controller, without depending on any xHCI-
 * specific type. All operations block the calling task until complete (via
 * the host controller's own interrupt-driven completion path) rather than
 * returning asynchronously -- this kernel is cooperatively scheduled with
 * no async I/O model, so a synchronous vtable matches every other driver
 * interface in the kernel. */
/* Called from the host controller's interrupt-handling path (e.g. inside
 * xhci_irq(), see xhci.c's IRQ-context invariant comment) every time a
 * submitted interrupt-IN transfer completes -- so, unlike every other
 * usb_hc_ops_t operation, this runs asynchronously and must not block
 * (no control transfers, no waiting on another command/transfer) and
 * should do the minimum work needed (e.g. drivers/hid.c just decodes an
 * 8-byte report and calls vt_input_push()) before returning. `buf` is the
 * same buffer pointer passed to submit_interrupt_transfer() -- valid only
 * for the duration of this call, since the host controller re-arms the
 * same buffer for the next transfer immediately after this returns. */
typedef void (*usb_interrupt_callback_t)(uint32_t slot_id, uint8_t endpoint_address,
                                          const void *buf, uint16_t length, bool success,
                                          void *ctx);

typedef struct usb_hc_ops {
    /* Allocates a device slot. slot_type comes from the Supported Protocol
     * Capability covering the port being enumerated. Returns false and
     * leaves *out_slot_id unchanged on failure. */
    bool (*enable_slot)(uint32_t slot_type, uint32_t *out_slot_id);

    /* Performs the full device-addressing sequence for a slot obtained
     * from enable_slot() -- on xHCI this is the two-stage BSR Address
     * Device dance (see docs/usb.md), opaque to callers here. */
    bool (*address_device)(uint32_t slot_id, uint32_t port, uint32_t speed);

    /* Issues one control transfer on the given slot's default control
     * pipe (EP0) and blocks until it completes. `data`/`data_in` are
     * ignored when wLength is 0. Returns false on timeout or a non-Success
     * completion code (both logged by the implementation). */
    bool (*control_transfer)(uint32_t slot_id, uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index, uint16_t w_length,
                              void *data, bool data_in);

    /* Configures one additional Interrupt IN endpoint on an already-
     * addressed slot (Configure Endpoint command on xHCI) and allocates
     * its transfer ring. `endpoint_address`/`max_packet_size`/`interval`
     * come straight from the device's own endpoint descriptor (see
     * usb_endpoint_descriptor_t). Bulk/Isoch endpoints are out of scope --
     * only Interrupt IN is supported, matching this driver's HID-boot-
     * keyboard-only class support. */
    bool (*configure_endpoint)(uint32_t slot_id, uint8_t endpoint_address,
                                uint16_t max_packet_size, uint8_t interval);

    /* Arms a repeating interrupt-IN transfer on an endpoint already set up
     * by configure_endpoint(): submits one transfer for `buf`/`length`
     * immediately, and re-arms the same buffer again automatically after
     * every completion (calling `callback` each time first) -- the
     * "keep polling forever" model a boot keyboard's report endpoint
     * needs, as opposed to control_transfer()'s one-shot blocking wait.
     * `buf` must remain valid for as long as the transfer stays armed
     * (i.e. for the life of the device attachment); there is no way to
     * disarm it once submitted (not needed by this driver's boot-time-
     * only device lifetime -- no hot-unplug handling exists yet). Returns
     * false if the initial submission fails; `callback` is never called
     * in that case. */
    bool (*submit_interrupt_transfer)(uint32_t slot_id, uint8_t endpoint_address, void *buf,
                                       uint16_t length, usb_interrupt_callback_t callback,
                                       void *ctx);
} usb_hc_ops_t;

/* GET_DESCRIPTOR convenience wrapper over hc->control_transfer(), used by
 * both the generic device-descriptor read below and (in a later milestone)
 * configuration/string descriptor reads. */
bool usb_get_descriptor(const usb_hc_ops_t *hc, uint32_t slot_id, uint8_t desc_type,
                         uint8_t desc_index, uint16_t language_id, void *buf, uint16_t length);

/* Generic enumeration for one already-connected, already-reset port:
 * Enable Slot -> Address Device -> device descriptor -> configuration
 * descriptor (short read for wTotalLength, then a full read, parsed for
 * every interface/endpoint) -> manufacturer/product strings (if the device
 * has any) -> SET_CONFIGURATION -> Configure Endpoint for the first
 * Interrupt IN endpoint found, if any -- logging every stage per
 * docs/usb.md's diagnostic requirements (device connected/reset are logged
 * by the caller; slot assigned, address assigned, VID/PID, manufacturer,
 * product, configuration selected are all logged here). Port connect-
 * detection and reset are xHCI register-level operations and stay in
 * xhci.c's xhci_enumerate(), which calls this once per connected port.
 *
 * `out_device` (may be NULL) receives everything learned about the device
 * regardless of how far enumeration got -- callers should check the
 * return value, not just inspect fields, to know whether enumeration
 * completed. Returns false (already logged) if any stage fails. */
bool usb_enumerate_port(const usb_hc_ops_t *hc, uint32_t port, uint32_t speed, uint32_t slot_type,
                         usb_device_t *out_device);

#endif
