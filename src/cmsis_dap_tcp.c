#define _GNU_SOURCE

#include "cmsis_dap.h"
#include "gpio_dap.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    default_port = 4441,
    default_swclk_pin = 11,
    default_swdio_pin = 10,
    default_clock_hz = 128000000U,
    tcp_header_size = 8U,
    packet_type_request = 1U,
    packet_type_response = 2U,
};

static const uint32_t packet_signature = UINT32_C(0x00504144); /* "DAP" */

static uint16_t load_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8U);
}

static uint32_t load_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static void store_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}

static void store_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static bool read_all(int fd, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    while (length != 0U) {
        ssize_t received = recv(fd, bytes, length, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (received == 0) {
            return false;
        }
        bytes += (size_t)received;
        length -= (size_t)received;
    }
    return true;
}

static bool write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    while (length != 0U) {
        ssize_t written = send(fd, bytes, length, MSG_NOSIGNAL);
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

static bool parse_unsigned(const char *text,
                           unsigned long minimum,
                           unsigned long maximum,
                           unsigned long *value)
{
    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static int listen_socket(const char *bind_address, unsigned port)
{
    char port_text[16];
    (void)snprintf(port_text, sizeof(port_text), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_PASSIVE,
    };
    struct addrinfo *addresses = NULL;
    int result = getaddrinfo(bind_address, port_text, &hints, &addresses);
    if (result != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(result));
        return -1;
    }

    int server = -1;
    for (struct addrinfo *address = addresses;
         address != NULL;
         address = address->ai_next) {
        server = socket(address->ai_family,
                        address->ai_socktype,
                        address->ai_protocol);
        if (server < 0) {
            continue;
        }
        int reuse = 1;
        (void)setsockopt(server,
                         SOL_SOCKET,
                         SO_REUSEADDR,
                         &reuse,
                         sizeof(reuse));
        if (bind(server, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(server, 1) == 0) {
            break;
        }
        (void)close(server);
        server = -1;
    }
    freeaddrinfo(addresses);
    return server;
}

static bool handle_client(int client, struct gpio_dap_backend *gpio_backend)
{
    struct cmsis_dap dap;
    struct cmsis_dap_backend backend = gpio_dap_make_backend(gpio_backend);
    cmsis_dap_init(&dap, &backend);

    uint8_t header[tcp_header_size];
    uint8_t request[cmsis_dap_packet_size];
    uint8_t response[cmsis_dap_packet_size];
    while (read_all(client, header, sizeof(header))) {
        uint32_t signature = load_u32(header);
        uint16_t length = load_u16(header + 4U);
        if (signature != packet_signature ||
            header[6] != packet_type_request ||
            header[7] != 0U ||
            length == 0U || length > cmsis_dap_packet_size) {
            fprintf(stderr, "invalid CMSIS-DAP TCP request header\n");
            break;
        }
        if (!read_all(client, request, length)) {
            break;
        }

        size_t response_length = cmsis_dap_process(&dap,
                                                   request,
                                                   length,
                                                   response,
                                                   sizeof(response));
        if (response_length == 0U || response_length > UINT16_MAX) {
            fprintf(stderr, "CMSIS-DAP command 0x%02x failed\n", request[0]);
            break;
        }

        store_u32(header, packet_signature);
        store_u16(header + 4U, (uint16_t)response_length);
        header[6] = packet_type_response;
        header[7] = 0U;
        if (!write_all(client, header, sizeof(header)) ||
            !write_all(client, response, response_length)) {
            break;
        }
    }
    if (dap.connected && backend.disconnect != NULL) {
        backend.disconnect(backend.ctx);
    }
    return true;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--bind ADDRESS] [--port PORT] [--swclk PIN] "
            "[--swdio PIN] [--clock HZ]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *bind_address = "127.0.0.1";
    unsigned port = default_port;
    unsigned swclk_pin = default_swclk_pin;
    unsigned swdio_pin = default_swdio_pin;
    unsigned clock_hz = default_clock_hz;

    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            bind_address = argv[++index];
            continue;
        }
        if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            unsigned long value = 0U;
            if (!parse_unsigned(argv[++index], 1U, 65535U, &value)) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            port = (unsigned)value;
            continue;
        }
        if (strcmp(argv[index], "--swclk") == 0 && index + 1 < argc) {
            unsigned long value = 0U;
            if (!parse_unsigned(argv[++index], 0U, 53U, &value)) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            swclk_pin = (unsigned)value;
            continue;
        }
        if (strcmp(argv[index], "--swdio") == 0 && index + 1 < argc) {
            unsigned long value = 0U;
            if (!parse_unsigned(argv[++index], 0U, 53U, &value)) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            swdio_pin = (unsigned)value;
            continue;
        }
        if (strcmp(argv[index], "--clock") == 0 && index + 1 < argc) {
            unsigned long value = 0U;
            if (!parse_unsigned(argv[++index], 1U, UINT32_MAX, &value)) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            clock_hz = (unsigned)value;
            continue;
        }
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    struct gpio_dap_backend gpio_backend = {0};
    int result = gpio_dap_open(&gpio_backend, swclk_pin, swdio_pin, clock_hz);
    if (result != 0) {
        fprintf(stderr, "cannot open GPIO SWD backend: %s\n", strerror(result));
        return EXIT_FAILURE;
    }

    int server = listen_socket(bind_address, port);
    if (server < 0) {
        perror("CMSIS-DAP TCP listen");
        gpio_dap_close(&gpio_backend);
        return EXIT_FAILURE;
    }
    fprintf(stderr,
            "CMSIS-DAP TCP listening on %s:%u, SWCLK GPIO%u, SWDIO GPIO%u, "
            "clock %u Hz\n",
            bind_address,
            port,
            swclk_pin,
            swdio_pin,
            clock_hz);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("CMSIS-DAP TCP accept");
            break;
        }
        int one = 1;
        (void)setsockopt(client,
                         IPPROTO_TCP,
                         TCP_NODELAY,
                         &one,
                         sizeof(one));
        fprintf(stderr, "CMSIS-DAP TCP client connected\n");
        (void)handle_client(client, &gpio_backend);
        (void)close(client);
        fprintf(stderr, "CMSIS-DAP TCP client disconnected\n");
    }

    (void)close(server);
    gpio_dap_close(&gpio_backend);
    return EXIT_FAILURE;
}
