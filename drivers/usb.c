#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/usb.h>

bool usb_get_descriptor(const usb_hc_ops_t *hc, uint32_t slot_id, uint8_t desc_type,
                         uint8_t desc_index, uint16_t language_id, void *buf, uint16_t length)
{
    uint16_t w_value = (uint16_t)(((uint16_t)desc_type << 8) | desc_index);
    return hc->control_transfer(slot_id,
                                 USB_REQUEST_TYPE_DEVICE_TO_HOST | USB_REQUEST_TYPE_STANDARD
                                     | USB_REQUEST_TYPE_RECIPIENT_DEVICE,
                                 USB_REQ_GET_DESCRIPTOR, w_value, language_id, length, buf, true);
}

/* Lossily decodes a UTF-16LE string descriptor body (the bytes following
 * the 2-byte bLength/bDescriptorType header) to ASCII: every code unit
 * outside 0-127 becomes '?'. Adequate for logging a manufacturer/product
 * name -- this driver has no need to render non-ASCII text anywhere. */
static void decode_string_ascii(const uint8_t *body, uint8_t body_len, char *out, uint8_t out_size)
{
    uint8_t units = (uint8_t)(body_len / 2);
    uint8_t i = 0;
    for (; i < units && i + 1 < out_size; ++i) {
        uint16_t code_unit = (uint16_t)(body[i * 2] | ((uint16_t)body[i * 2 + 1] << 8));
        out[i] = (code_unit < 128U) ? (char)code_unit : '?';
    }
    out[i] = '\0';
}

static bool read_string(const usb_hc_ops_t *hc, uint32_t slot_id, uint8_t index, char *out,
                         uint8_t out_size)
{
    out[0] = '\0';
    if (index == 0) {
        return false;
    }

    usb_string_descriptor_header_t hdr;
    if (!usb_get_descriptor(hc, slot_id, USB_DESC_TYPE_STRING, index, USB_LANGID_US_ENGLISH, &hdr,
                             sizeof(hdr))
        || hdr.bLength < sizeof(hdr)) {
        return false;
    }

    uint8_t buf[255]; /* bLength is a single byte -- 255 is the hard maximum */
    if (!usb_get_descriptor(hc, slot_id, USB_DESC_TYPE_STRING, index, USB_LANGID_US_ENGLISH, buf,
                             hdr.bLength)) {
        return false;
    }
    decode_string_ascii(buf + sizeof(usb_string_descriptor_header_t),
                         (uint8_t)(hdr.bLength - sizeof(usb_string_descriptor_header_t)), out,
                         out_size);
    return true;
}

/* Walks a raw configuration-descriptor blob (config header + every
 * interface/endpoint descriptor that follows it, exactly as the device
 * returned it) and records the first Interrupt IN endpoint found, plus
 * logs every interface/endpoint encountered along the way. Descriptors
 * this driver doesn't care about (HID class descriptors, unrecognized
 * vendor-specific ones, ...) are skipped generically via their own
 * bLength -- this driver never needs to know their internal layout to
 * walk past them. */
static void parse_configuration(const uint8_t *buf, uint16_t total_length, usb_device_t *dev)
{
    uint16_t offset = 0;
    uint8_t current_interface = 0;
    uint8_t current_class = 0;
    uint8_t current_subclass = 0;
    uint8_t current_protocol = 0;

    while ((uint16_t)(offset + 2) <= total_length) {
        uint8_t b_length = buf[offset];
        uint8_t b_type = buf[offset + 1];
        if (b_length < 2 || (uint16_t)(offset + b_length) > total_length) {
            break;
        }

        if (b_type == USB_DESC_TYPE_INTERFACE && b_length >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *iface =
                (const usb_interface_descriptor_t *)(buf + offset);
            current_interface = iface->bInterfaceNumber;
            current_class = iface->bInterfaceClass;
            current_subclass = iface->bInterfaceSubClass;
            current_protocol = iface->bInterfaceProtocol;
            printf("usb: interface %u: class=%02x subclass=%02x protocol=%02x\n",
                   current_interface, current_class, current_subclass, current_protocol);
        } else if (b_type == USB_DESC_TYPE_ENDPOINT
                   && b_length >= sizeof(usb_endpoint_descriptor_t)) {
            const usb_endpoint_descriptor_t *ep = (const usb_endpoint_descriptor_t *)(buf + offset);
            printf("usb: endpoint %02x: attributes=%02x max_packet=%u interval=%u\n",
                   ep->bEndpointAddress, ep->bmAttributes, ep->wMaxPacketSize & 0x7FFU,
                   ep->bInterval);
            if (!dev->has_interrupt_in_endpoint
                && USB_ENDPOINT_ATTR_TYPE(ep->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT
                && USB_ENDPOINT_ADDRESS_IS_IN(ep->bEndpointAddress)) {
                dev->has_interrupt_in_endpoint = true;
                dev->interface_number = current_interface;
                dev->interface_class = current_class;
                dev->interface_subclass = current_subclass;
                dev->interface_protocol = current_protocol;
                dev->endpoint_address = ep->bEndpointAddress;
                dev->endpoint_max_packet_size = (uint16_t)(ep->wMaxPacketSize & 0x7FFU);
                dev->endpoint_interval = ep->bInterval;
            }
            /* Independent of the interrupt-IN capture above (sibling `if`,
             * not `else if`) -- a device can only ever be one or the other
             * in practice, but nothing here assumes that. First Bulk IN and
             * first Bulk OUT found on a Mass-Storage-class interface become
             * the pipes drivers/usb_msd.c's Bulk-Only Transport uses; a
             * bEndpointAddress of 0 is never valid for a real bulk
             * endpoint (EP0 is always Control), so it doubles as the
             * "not yet captured" sentinel below. */
            if (current_class == USB_CLASS_MASS_STORAGE
                && USB_ENDPOINT_ATTR_TYPE(ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
                if (USB_ENDPOINT_ADDRESS_IS_IN(ep->bEndpointAddress) && dev->bulk_in_addr == 0) {
                    dev->bulk_in_addr = ep->bEndpointAddress;
                    dev->bulk_in_max_packet = (uint16_t)(ep->wMaxPacketSize & 0x7FFU);
                    dev->msd_interface_number = current_interface;
                } else if (!USB_ENDPOINT_ADDRESS_IS_IN(ep->bEndpointAddress)
                           && dev->bulk_out_addr == 0) {
                    dev->bulk_out_addr = ep->bEndpointAddress;
                    dev->bulk_out_max_packet = (uint16_t)(ep->wMaxPacketSize & 0x7FFU);
                }
                if (dev->bulk_in_addr != 0 && dev->bulk_out_addr != 0) {
                    dev->has_bulk_endpoints = true;
                }
            }
        }

        offset = (uint16_t)(offset + b_length);
    }
}

bool usb_enumerate_port(const usb_hc_ops_t *hc, uint32_t port, uint32_t speed, uint32_t slot_type,
                         usb_device_t *out_device)
{
    usb_device_t dev;
    memset(&dev, 0, sizeof(dev));

    uint32_t slot_id = 0;
    if (!hc->enable_slot(slot_type, &slot_id)) {
        printf("usb: port %u: failed to enable a device slot\n", port);
        if (out_device) {
            *out_device = dev;
        }
        return false;
    }
    dev.slot_id = slot_id;
    printf("usb: port %u: slot %u assigned\n", port, slot_id);

    if (!hc->address_device(slot_id, port, speed)) {
        printf("usb: slot %u: failed to address device\n", slot_id);
        if (out_device) {
            *out_device = dev;
        }
        return false;
    }
    printf("usb: slot %u: address assigned\n", slot_id);

    usb_device_descriptor_t desc;
    if (!usb_get_descriptor(hc, slot_id, USB_DESC_TYPE_DEVICE, 0, 0, &desc, sizeof(desc))) {
        printf("usb: slot %u: failed to read device descriptor\n", slot_id);
        if (out_device) {
            *out_device = dev;
        }
        return false;
    }
    dev.vid = desc.idVendor;
    dev.pid = desc.idProduct;
    dev.device_class = desc.bDeviceClass;
    dev.device_subclass = desc.bDeviceSubClass;
    dev.device_protocol = desc.bDeviceProtocol;
    printf("usb: slot %u: VID=%04x PID=%04x class=%02x subclass=%02x protocol=%02x "
           "configs=%u\n",
           slot_id, dev.vid, dev.pid, dev.device_class, dev.device_subclass, dev.device_protocol,
           desc.bNumConfigurations);

    usb_config_descriptor_t cfg_header;
    if (!usb_get_descriptor(hc, slot_id, USB_DESC_TYPE_CONFIGURATION, 0, 0, &cfg_header,
                             sizeof(cfg_header))) {
        printf("usb: slot %u: failed to read configuration descriptor header\n", slot_id);
        if (out_device) {
            *out_device = dev;
        }
        return false;
    }

    usb_debugf("usb: slot %u: configuration descriptor header: wTotalLength=%u "
               "bNumInterfaces=%u bConfigurationValue=%u\n",
               slot_id, cfg_header.wTotalLength, cfg_header.bNumInterfaces,
               cfg_header.bConfigurationValue);

    uint16_t total_length = cfg_header.wTotalLength;
    if (total_length < sizeof(cfg_header)) {
        total_length = sizeof(cfg_header);
    }
    if (total_length > USB_MAX_CONFIG_DESC_SIZE) {
        printf("usb: slot %u: configuration descriptor (%u bytes) exceeds this driver's "
               "%u-byte parse buffer; truncating\n",
               slot_id, total_length, USB_MAX_CONFIG_DESC_SIZE);
        total_length = USB_MAX_CONFIG_DESC_SIZE;
    }

    uint8_t config_buf[USB_MAX_CONFIG_DESC_SIZE];
    if (!usb_get_descriptor(hc, slot_id, USB_DESC_TYPE_CONFIGURATION, 0, 0, config_buf,
                             total_length)) {
        printf("usb: slot %u: failed to read full configuration descriptor\n", slot_id);
        if (out_device) {
            *out_device = dev;
        }
        return false;
    }
    parse_configuration(config_buf, total_length, &dev);

    if (read_string(hc, slot_id, desc.iManufacturer, dev.manufacturer, sizeof(dev.manufacturer))) {
        printf("usb: slot %u: manufacturer=\"%s\"\n", slot_id, dev.manufacturer);
    }
    if (read_string(hc, slot_id, desc.iProduct, dev.product, sizeof(dev.product))) {
        printf("usb: slot %u: product=\"%s\"\n", slot_id, dev.product);
    }

    if (!hc->control_transfer(slot_id,
                               USB_REQUEST_TYPE_HOST_TO_DEVICE | USB_REQUEST_TYPE_STANDARD
                                   | USB_REQUEST_TYPE_RECIPIENT_DEVICE,
                               USB_REQ_SET_CONFIGURATION, cfg_header.bConfigurationValue, 0, 0,
                               NULL, false)) {
        printf("usb: slot %u: SET_CONFIGURATION(%u) failed\n", slot_id,
               cfg_header.bConfigurationValue);
        if (out_device) {
            *out_device = dev;
        }
        return false;
    }
    printf("usb: slot %u: configuration %u selected\n", slot_id, cfg_header.bConfigurationValue);

    if (dev.has_interrupt_in_endpoint) {
        if (hc->configure_endpoint(slot_id, dev.endpoint_address, dev.endpoint_max_packet_size,
                                    dev.endpoint_interval)) {
            printf("usb: slot %u: interrupt endpoint %02x configured (interface %u, "
                   "max_packet=%u)\n",
                   slot_id, dev.endpoint_address, dev.interface_number,
                   dev.endpoint_max_packet_size);
        } else {
            printf("usb: slot %u: failed to configure interrupt endpoint %02x\n", slot_id,
                   dev.endpoint_address);
        }
    }

    if (dev.has_bulk_endpoints) {
        if (hc->configure_bulk_endpoints(slot_id, dev.bulk_in_addr, dev.bulk_in_max_packet,
                                          dev.bulk_out_addr, dev.bulk_out_max_packet)) {
            printf("usb: slot %u: bulk endpoints configured (in=%02x out=%02x, interface %u)\n",
                   slot_id, dev.bulk_in_addr, dev.bulk_out_addr, dev.msd_interface_number);
        } else {
            printf("usb: slot %u: failed to configure bulk endpoints (in=%02x out=%02x)\n",
                   slot_id, dev.bulk_in_addr, dev.bulk_out_addr);
            /* So a class driver checking has_bulk_endpoints later (e.g.
             * usb_msd_try_attach()) doesn't try to use pipes that were
             * never actually set up. */
            dev.has_bulk_endpoints = false;
        }
    }

    if (out_device) {
        *out_device = dev;
    }
    return true;
}
