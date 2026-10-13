#define _GNU_SOURCE

#include "cmsis_dap.h"
#include "gpio_dap.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

enum {
    interface_class_vendor = 0xff,
    endpoint_out = 0x01,
    endpoint_in = 0x82,
    bulk_attributes = 0x02,
};

/* The supported host is a little-endian Raspberry Pi, and FunctionFS
 * descriptors are specified as little-endian fields. */
#define DAP_LE16(value) ((uint16_t)(value))
#define DAP_LE32(value) ((uint32_t)(value))

struct dap_descriptors {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count;
    __le32 hs_count;
    struct usb_interface_descriptor fs_interface;
    struct usb_endpoint_descriptor_no_audio fs_out;
    struct usb_endpoint_descriptor_no_audio fs_in;
    struct usb_interface_descriptor hs_interface;
    struct usb_endpoint_descriptor_no_audio hs_out;
    struct usb_endpoint_descriptor_no_audio hs_in;
} __attribute__((packed));

static const struct dap_descriptors descriptors = {
    .header = {
        .magic = DAP_LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
        .length = DAP_LE32(sizeof(struct dap_descriptors)),
        .flags = DAP_LE32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC),
    },
    .fs_count = DAP_LE32(3U),
    .hs_count = DAP_LE32(3U),
    .fs_interface = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0U,
        .bAlternateSetting = 0U,
        .bNumEndpoints = 2U,
        .bInterfaceClass = interface_class_vendor,
        .bInterfaceSubClass = 0U,
        .bInterfaceProtocol = 0U,
        .iInterface = 0U,
    },
    .fs_out = {
        .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = endpoint_out,
        .bmAttributes = bulk_attributes,
        .wMaxPacketSize = DAP_LE16(64U),
        .bInterval = 0U,
    },
    .fs_in = {
        .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = endpoint_in,
        .bmAttributes = bulk_attributes,
        .wMaxPacketSize = DAP_LE16(64U),
        .bInterval = 0U,
    },
    .hs_interface = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = 0U,
        .bAlternateSetting = 0U,
        .bNumEndpoints = 2U,
        .bInterfaceClass = interface_class_vendor,
        .bInterfaceSubClass = 0U,
        .bInterfaceProtocol = 0U,
        .iInterface = 0U,
    },
    .hs_out = {
        .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = endpoint_out,
        .bmAttributes = bulk_attributes,
        .wMaxPacketSize = DAP_LE16(512U),
        .bInterval = 0U,
    },
    .hs_in = {
        .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = endpoint_in,
        .bmAttributes = bulk_attributes,
        .wMaxPacketSize = DAP_LE16(512U),
        .bInterval = 0U,
    },
};

static bool write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    while (length != 0U) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        bytes += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

static void handle_event(const struct usb_functionfs_event *event,
                         bool *enabled,
                         bool *running)
{
    switch (event->type) {
    case FUNCTIONFS_ENABLE:
        *enabled = true;
        fprintf(stderr, "FunctionFS enabled\n");
        break;
    case FUNCTIONFS_DISABLE:
        *enabled = false;
        fprintf(stderr, "FunctionFS disabled\n");
        break;
    case FUNCTIONFS_UNBIND:
        *enabled = false;
        *running = false;
        fprintf(stderr, "FunctionFS unbound\n");
        break;
    case FUNCTIONFS_BIND:
        fprintf(stderr, "FunctionFS bound\n");
        break;
    case FUNCTIONFS_SETUP:
        /* No class-specific control requests are required by CMSIS-DAPv2. */
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    const char *functionfs_path = argc > 1 ? argv[1] : "/dev/ffs/cmsis-dap";
    char ep0_path[256];
    char ep_out_path[256];
    char ep_in_path[256];
    (void)snprintf(ep0_path, sizeof(ep0_path), "%s/ep0", functionfs_path);
    (void)snprintf(ep_out_path, sizeof(ep_out_path), "%s/ep1", functionfs_path);
    (void)snprintf(ep_in_path, sizeof(ep_in_path), "%s/ep2", functionfs_path);

    int ep0 = open(ep0_path, O_RDWR | O_CLOEXEC);
    if (ep0 < 0) {
        perror(ep0_path);
        return EXIT_FAILURE;
    }
    if (!write_all(ep0, &descriptors, sizeof(descriptors))) {
        perror("FunctionFS descriptors");
        (void)close(ep0);
        return EXIT_FAILURE;
    }
    int ep_out = open(ep_out_path, O_RDWR | O_CLOEXEC);
    int ep_in = open(ep_in_path, O_RDWR | O_CLOEXEC);
    if (ep_out < 0 || ep_in < 0) {
        perror("FunctionFS endpoints");
        if (ep_out >= 0) {
            (void)close(ep_out);
        }
        if (ep_in >= 0) {
            (void)close(ep_in);
        }
        (void)close(ep0);
        return EXIT_FAILURE;
    }

    struct gpio_dap_backend gpio_backend = {0};
    if (gpio_dap_open(&gpio_backend, 11U, 10U, 128000000U) != 0) {
        fprintf(stderr, "cannot open GPIO SWD backend\n");
        (void)close(ep_in);
        (void)close(ep_out);
        (void)close(ep0);
        return EXIT_FAILURE;
    }
    struct cmsis_dap dap;
    struct cmsis_dap_backend backend = gpio_dap_make_backend(&gpio_backend);
    cmsis_dap_init(&dap, &backend);
    uint8_t request[cmsis_dap_packet_size];
    uint8_t response[cmsis_dap_packet_size];
    bool enabled = false;
    bool running = true;
    while (running) {
        struct pollfd fds[2] = {
            {.fd = ep0, .events = POLLIN},
            {.fd = ep_out, .events = enabled ? POLLIN : 0},
        };
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("FunctionFS poll");
            break;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            struct usb_functionfs_event event = {0};
            ssize_t count = read(ep0, &event, sizeof(event));
            if (count == (ssize_t)sizeof(event)) {
                handle_event(&event, &enabled, &running);
            }
        }
        if (enabled && (fds[1].revents & POLLIN) != 0) {
            ssize_t count = read(ep_out, request, sizeof(request));
            if (count <= 0) {
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            size_t response_length = cmsis_dap_process(&dap,
                                                       request,
                                                       (size_t)count,
                                                       response,
                                                       sizeof(response));
            if (response_length == 0U ||
                !write_all(ep_in, response, response_length)) {
                perror("FunctionFS response");
                break;
            }
        }
    }
    gpio_dap_close(&gpio_backend);
    (void)close(ep_in);
    (void)close(ep_out);
    (void)close(ep0);
    return EXIT_SUCCESS;
}
