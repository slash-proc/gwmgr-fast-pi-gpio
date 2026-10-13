#define _GNU_SOURCE

/*
 * Small GDB Remote Serial Protocol server for the native GPIO/SWD engine.
 *
 * This is intentionally a narrow compatibility server rather than a full
 * debugger. It implements the operations used by GnWManager's GDB backend and
 * translates them directly into batched SWD memory operations. Keeping the
 * server here means the timing-critical GPIO code remains shared with the
 * CMSIS-DAP transport and no OpenOCD process is required on the Pi.
 */

#include "gpio_dap.h"

#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    default_port = 3333,
    default_swclk_pin = 11,
    default_swdio_pin = 10,
    default_clock_hz = 128000000,
    max_packet_size = 262144,
    max_memory_request = 65536,
    core_register_count = 17,
    m_system_register_count = 6,
    gdb_register_count = core_register_count + m_system_register_count,
    read_cache_capacity = 64U * 1024U,
};

static const uint32_t external_flash_base = UINT32_C(0x90000000);
static const uint32_t external_flash_limit = UINT32_C(0xa0000000);

/* Keep the core feature and M-system feature separate, as GWemu does. This
 * lets a regular GDB client resolve the optional system registers from the
 * feature annex while keeping the existing Cortex-M core register numbers.
 * xPSR uses the standard M-profile RSP register number 25; p10 remains an
 * alias for existing minimal clients such as GnWManager. */
static const char target_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target xmlns:xi=\"http://www.w3.org/2001/XInclude\">"
    "<architecture>arm</architecture>"
    "<xi:include href=\"arm-m-profile.xml\"/>"
    "<xi:include href=\"arm-m-system.xml\"/>"
    "</target>";

static const char arm_m_profile_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE feature SYSTEM \"gdb-target.dtd\">"
    "<feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\" regnum=\"0\"/>"
    "<reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/>"
    "<reg name=\"r5\" bitsize=\"32\"/><reg name=\"r6\" bitsize=\"32\"/>"
    "<reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/>"
    "<reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/>"
    "<reg name=\"r11\" bitsize=\"32\"/><reg name=\"r12\" bitsize=\"32\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/><reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\" regnum=\"25\"/>"
    "</feature>";

static const char arm_m_system_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE feature SYSTEM \"gdb-target.dtd\">"
    "<feature name=\"org.gnu.gdb.arm.m-system\">"
    "<reg name=\"msp\" bitsize=\"32\" regnum=\"26\" type=\"int\"/>"
    "<reg name=\"psp\" bitsize=\"32\" regnum=\"27\" type=\"int\"/>"
    "<reg name=\"primask\" bitsize=\"32\" regnum=\"28\" type=\"int\"/>"
    "<reg name=\"control\" bitsize=\"32\" regnum=\"29\" type=\"int\"/>"
    "<reg name=\"basepri\" bitsize=\"32\" regnum=\"30\" type=\"int\"/>"
    "<reg name=\"faultmask\" bitsize=\"32\" regnum=\"31\" type=\"int\"/>"
    "</feature>";

static const unsigned m_system_register_indices[m_system_register_count] = {
    gpio_dap_msp_register,
    gpio_dap_psp_register,
    gpio_dap_primask_register,
    gpio_dap_control_register,
    gpio_dap_basepri_register,
    gpio_dap_faultmask_register,
};

enum packet_result {
    packet_closed = 0,
    packet_ready = 1,
    packet_interrupt = 2,
    packet_retry = 3,
};

struct gdb_target {
    struct gpio_dap_backend dap;
    bool attached;
    bool running;
    bool debug;
    size_t read_cache_size;
    bool read_cache_valid;
    uint32_t read_cache_address;
    uint8_t read_cache[read_cache_capacity];
    uint32_t idcode;
    struct {
        uint64_t packets;
        uint64_t read_requests;
        uint64_t read_bytes;
        uint64_t read_windows;
        uint64_t write_requests;
        uint64_t write_bytes;
        uint64_t read_ns;
        uint64_t read_receive_ns;
        uint64_t read_encode_ns;
        uint64_t read_send_ns;
        uint64_t read_total_ns;
        uint64_t write_ns;
        uint64_t cache_hits;
        uint64_t cache_misses;
        struct timespec started;
        struct timespec packet_started;
        uint64_t packet_receive_ns;
    } stats;
};

static uint64_t elapsed_ns(struct timespec start, struct timespec end)
{
    int64_t seconds = (int64_t)end.tv_sec - (int64_t)start.tv_sec;
    int64_t nanoseconds = (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;
    return (uint64_t)(seconds * INT64_C(1000000000) + nanoseconds);
}

static size_t read_window_count(uint32_t address, size_t length)
{
    size_t windows = 0U;
    while (length != 0U) {
        size_t window = 1024U - ((size_t)address & 1023U);
        if (window > length) {
            window = length;
        }
        address += (uint32_t)window;
        length -= window;
        windows++;
    }
    return windows;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --bind ADDRESS    listen address (default: 127.0.0.1)\n"
            "  --port PORT       GDB TCP port (default: 3333)\n"
            "  --swclk PIN       SWCLK GPIO (default: 11)\n"
            "  --swdio PIN       SWDIO GPIO (default: 10)\n"
            "  --clock HZ        requested SWCLK (default: 128000000)\n"
            "  --debug           print per-client GDB/SWD throughput statistics\n"
            "  --cache-read-kib N  cache aligned N KiB read blocks (debug experiment)\n",
            program);
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

static bool send_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
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

static bool send_ack(int fd, uint8_t value)
{
    return send_all(fd, &value, 1U);
}

static char hex_digit(unsigned value)
{
    return value < 10U ? (char)('0' + value) : (char)('a' + value - 10U);
}

static int hex_value(uint8_t value)
{
    if (value >= '0' && value <= '9') {
        return (int)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return (int)(value - 'a' + 10U);
    }
    if (value >= 'A' && value <= 'F') {
        return (int)(value - 'A' + 10U);
    }
    return -1;
}

static bool send_packet(int fd, const uint8_t *payload, size_t length)
{
    if (length > max_packet_size) {
        return false;
    }
    /* Build the complete escaped packet before sending it. Sending each
     * payload byte separately makes a large hex-encoded memory response pay
     * one syscall per byte, overwhelming the actual SWD transfer. */
    size_t capacity = length * 2U + 4U;
    uint8_t *wire = malloc(capacity);
    if (wire == NULL) {
        return false;
    }

    size_t wire_length = 0U;
    wire[wire_length++] = '$';
    unsigned checksum = 0U;
    for (size_t index = 0U; index < length; ++index) {
        uint8_t value = payload[index];
        checksum = (checksum + value) & 0xffU;
        if (value == '$' || value == '#' || value == '}' || value == '*') {
            wire[wire_length++] = '}';
            wire[wire_length++] = (uint8_t)(value ^ 0x20U);
        } else {
            wire[wire_length++] = value;
        }
    }
    wire[wire_length++] = '#';
    wire[wire_length++] = (uint8_t)hex_digit(checksum >> 4U);
    wire[wire_length++] = (uint8_t)hex_digit(checksum & 0x0fU);

    bool success = send_all(fd, wire, wire_length);
    free(wire);
    return success;
}

static bool send_text_packet(int fd, const char *text)
{
    return send_packet(fd, (const uint8_t *)text, strlen(text));
}

static bool send_error(int fd, unsigned code)
{
    char response[4];
    (void)snprintf(response, sizeof(response), "E%02x", code & 0xffU);
    return send_text_packet(fd, response);
}

static bool read_byte(int fd, uint8_t *value)
{
    for (;;) {
        ssize_t received = recv(fd, value, 1U, 0);
        if (received > 0) {
            return true;
        }
        if (received == 0) {
            return false;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

static enum packet_result receive_packet(int fd,
                                         uint8_t *payload,
                                         size_t capacity,
                                         size_t *length)
{
    uint8_t value = 0U;
    for (;;) {
        if (!read_byte(fd, &value)) {
            return packet_closed;
        }
        if (value == 0x03U) {
            return packet_interrupt;
        }
        if (value == '$') {
            break;
        }
        /* Client acknowledgements and unrelated line noise are ignored. */
    }

    uint8_t wire[max_packet_size];
    size_t wire_length = 0U;
    for (;;) {
        if (!read_byte(fd, &value)) {
            return packet_closed;
        }
        if (value == '#') {
            break;
        }
        if (wire_length == sizeof(wire)) {
            (void)send_ack(fd, '-');
            return packet_retry;
        }
        wire[wire_length++] = value;
    }

    uint8_t checksum_bytes[2] = {0U, 0U};
    if (!read_byte(fd, &checksum_bytes[0]) ||
        !read_byte(fd, &checksum_bytes[1])) {
        return packet_closed;
    }
    int high = hex_value(checksum_bytes[0]);
    int low = hex_value(checksum_bytes[1]);
    if (high < 0 || low < 0) {
        (void)send_ack(fd, '-');
        return packet_retry;
    }

    unsigned checksum = 0U;
    for (size_t index = 0U; index < wire_length; ++index) {
        checksum = (checksum + wire[index]) & 0xffU;
    }
    if (checksum != (unsigned)((high << 4) | low)) {
        (void)send_ack(fd, '-');
        return packet_retry;
    }

    size_t decoded_length = 0U;
    for (size_t index = 0U; index < wire_length; ++index) {
        uint8_t decoded = wire[index];
        if (decoded == '}') {
            if (++index == wire_length) {
                (void)send_ack(fd, '-');
                return packet_retry;
            }
            decoded = (uint8_t)(wire[index] ^ 0x20U);
        }
        if (decoded_length == capacity) {
            (void)send_ack(fd, '-');
            return packet_retry;
        }
        payload[decoded_length++] = decoded;
    }
    if (!send_ack(fd, '+')) {
        return packet_closed;
    }
    *length = decoded_length;
    return packet_ready;
}

static bool parse_hex_u32(const uint8_t *text,
                          size_t length,
                          uint32_t *value)
{
    if (length == 0U || value == NULL) {
        return false;
    }
    uint64_t result = 0U;
    for (size_t index = 0U; index < length; ++index) {
        int digit = hex_value(text[index]);
        if (digit < 0) {
            return false;
        }
        result = (result << 4U) | (unsigned)digit;
        if (result > UINT32_MAX) {
            return false;
        }
    }
    *value = (uint32_t)result;
    return true;
}

static bool parse_hex_size(const uint8_t *text,
                           size_t length,
                           size_t *value)
{
    if (length == 0U || value == NULL) {
        return false;
    }
    uint64_t result = 0U;
    for (size_t index = 0U; index < length; ++index) {
        int digit = hex_value(text[index]);
        if (digit < 0) {
            return false;
        }
        result = (result << 4U) | (unsigned)digit;
        if (result > max_memory_request) {
            return false;
        }
    }
    *value = (size_t)result;
    return true;
}

static uint32_t load_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static void store_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static bool decode_hex_bytes(const uint8_t *text,
                             size_t text_length,
                             uint8_t *bytes,
                             size_t byte_length)
{
    if (text_length != byte_length * 2U) {
        return false;
    }
    for (size_t index = 0U; index < byte_length; ++index) {
        int high = hex_value(text[index * 2U]);
        int low = hex_value(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool encode_hex_bytes(const uint8_t *bytes,
                             size_t byte_length,
                             uint8_t *text)
{
    for (size_t index = 0U; index < byte_length; ++index) {
        text[index * 2U] = (uint8_t)hex_digit(bytes[index] >> 4U);
        text[index * 2U + 1U] = (uint8_t)hex_digit(bytes[index] & 0x0fU);
    }
    return true;
}

static bool read_memory_for_gdb(struct gdb_target *target,
                                uint32_t address,
                                uint8_t *buffer,
                                size_t length)
{
    if (length == 0U) {
        return true;
    }
    uint32_t aligned_address = address & ~UINT32_C(3);
    size_t prefix = (size_t)(address - aligned_address);
    size_t aligned_length = (prefix + length + 3U) & ~((size_t)3U);
    uint8_t *aligned = malloc(aligned_length);
    if (aligned == NULL) {
        return false;
    }
    bool success = gpio_dap_read_memory(target == NULL ? NULL : &target->dap,
                                        aligned_address,
                                        aligned,
                                        aligned_length);
    if (success) {
        memcpy(buffer, aligned + prefix, length);
    }
    free(aligned);
    return success;
}

static void invalidate_read_cache(struct gdb_target *target)
{
    target->read_cache_valid = false;
}

static bool read_memory_cached(struct gdb_target *target,
                               uint32_t address,
                               uint8_t *buffer,
                               size_t length)
{
    uint64_t request_end = (uint64_t)address + length;
    if (target->read_cache_size == 0U || length > target->read_cache_size ||
        address < external_flash_base || request_end > external_flash_limit) {
        invalidate_read_cache(target);
        return read_memory_for_gdb(target, address, buffer, length);
    }

    uint32_t cache_base = address & ~((uint32_t)target->read_cache_size - 1U);
    uint64_t cache_end = (uint64_t)cache_base + target->read_cache_size;
    bool hit = target->read_cache_valid &&
               target->read_cache_address == cache_base &&
               request_end <= cache_end;
    if (!hit) {
        if (!read_memory_for_gdb(target,
                                 cache_base,
                                 target->read_cache,
                                 target->read_cache_size)) {
            invalidate_read_cache(target);
            return false;
        }
        target->read_cache_address = cache_base;
        target->read_cache_valid = true;
        target->stats.cache_misses++;
    } else {
        target->stats.cache_hits++;
    }
    memcpy(buffer, target->read_cache + (address - cache_base), length);
    return true;
}

static bool handle_memory_read(struct gdb_target *target,
                               int fd,
                               const uint8_t *packet,
                               size_t length)
{
    const uint8_t *comma = memchr(packet, ',', length);
    if (comma == NULL || comma == packet || comma + 1U >= packet + length) {
        return send_error(fd, 1U);
    }
    uint32_t address = 0U;
    size_t count = 0U;
    if (!parse_hex_u32(packet + 1U,
                       (size_t)(comma - (packet + 1U)),
                       &address) ||
        !parse_hex_size(comma + 1U,
                        (size_t)(packet + length - (comma + 1U)),
                        &count) ||
        count > max_memory_request) {
        return send_error(fd, 1U);
    }
    uint8_t *data = malloc(count == 0U ? 1U : count);
    uint8_t *encoded = malloc(count * 2U + 1U);
    if (data == NULL || encoded == NULL) {
        free(data);
        free(encoded);
        return send_error(fd, 2U);
    }
    struct timespec swd_start = {0};
    struct timespec swd_end = {0};
    struct timespec encode_start = {0};
    struct timespec encode_end = {0};
    struct timespec send_start = {0};
    struct timespec send_end = {0};
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &swd_start);
    }
    bool success = read_memory_cached(target, address, data, count);
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &swd_end);
        uint64_t elapsed = elapsed_ns(swd_start, swd_end);
        size_t windows = read_window_count(address, count);
        target->stats.read_requests++;
        target->stats.read_bytes += count;
        target->stats.read_windows += windows;
        target->stats.read_ns += elapsed;
    }
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &encode_start);
    }
    if (success) {
        (void)encode_hex_bytes(data, count, encoded);
        encoded[count * 2U] = '\0';
    } else {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &encode_end);
        if (target->debug) {
            (void)clock_gettime(CLOCK_MONOTONIC_RAW, &send_start);
        }
        success = send_error(fd, 1U);
        if (target->debug) {
            (void)clock_gettime(CLOCK_MONOTONIC_RAW, &send_end);
        }
        goto read_done;
    }
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &encode_end);
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &send_start);
    }
    success = send_text_packet(fd, (const char *)encoded);
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &send_end);
    }
read_done:
    if (target->debug) {
        uint64_t encode_ns = elapsed_ns(encode_start, encode_end);
        uint64_t send_ns = elapsed_ns(send_start, send_end);
        uint64_t total_ns = elapsed_ns(target->stats.packet_started, send_end);
        size_t windows = read_window_count(address, count);
        target->stats.read_receive_ns += target->stats.packet_receive_ns;
        target->stats.read_encode_ns += encode_ns;
        target->stats.read_send_ns += send_ns;
        target->stats.read_total_ns += total_ns;
        fprintf(stderr,
                "[gdb-debug] m addr=0x%08" PRIx32 " bytes=%zu windows=%zu total=%.3f ms receive=%.3f swd=%.3f encode=%.3f send=%.3f rate=%.1f KiB/s\n",
                address, count, windows, (double)total_ns / 1000000.0,
                (double)target->stats.packet_receive_ns / 1000000.0,
                (double)elapsed_ns(swd_start, swd_end) / 1000000.0,
                (double)encode_ns / 1000000.0,
                (double)send_ns / 1000000.0,
                total_ns == 0U ? 0.0 :
                    ((double)count * 1000000000.0 / (double)total_ns) / 1024.0);
    }
    free(data);
    free(encoded);
    return success;
}

static bool handle_memory_read_binary(struct gdb_target *target,
                                      int fd,
                                      const uint8_t *packet,
                                      size_t length)
{
    const uint8_t *comma = memchr(packet, ',', length);
    if (comma == NULL || comma == packet || comma + 1U >= packet + length) {
        return send_error(fd, 1U);
    }
    uint32_t address = 0U;
    size_t count = 0U;
    if (!parse_hex_u32(packet + 1U,
                       (size_t)(comma - (packet + 1U)),
                       &address) ||
        !parse_hex_size(comma + 1U,
                        (size_t)(packet + length - (comma + 1U)),
                        &count) ||
        count > max_memory_request) {
        return send_error(fd, 1U);
    }
    uint8_t *data = malloc(count == 0U ? 1U : count);
    if (data == NULL) {
        return send_error(fd, 2U);
    }
    struct timespec swd_start = {0};
    struct timespec swd_end = {0};
    struct timespec send_start = {0};
    struct timespec send_end = {0};
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &swd_start);
    }
    bool success = read_memory_cached(target, address, data, count);
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &swd_end);
        uint64_t elapsed = elapsed_ns(swd_start, swd_end);
        size_t windows = read_window_count(address, count);
        target->stats.read_requests++;
        target->stats.read_bytes += count;
        target->stats.read_windows += windows;
        target->stats.read_ns += elapsed;
    }
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &send_start);
    }
    if (success) {
        success = send_packet(fd, data, count);
    } else {
        success = send_error(fd, 1U);
    }
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &send_end);
        uint64_t send_ns = elapsed_ns(send_start, send_end);
        uint64_t total_ns = elapsed_ns(target->stats.packet_started, send_end);
        size_t windows = read_window_count(address, count);
        target->stats.read_receive_ns += target->stats.packet_receive_ns;
        target->stats.read_send_ns += send_ns;
        target->stats.read_total_ns += total_ns;
        fprintf(stderr,
                "[gdb-debug] x addr=0x%08" PRIx32 " bytes=%zu windows=%zu total=%.3f ms receive=%.3f swd=%.3f encode=0.000 send=%.3f rate=%.1f KiB/s\n",
                address, count, windows, (double)total_ns / 1000000.0,
                (double)target->stats.packet_receive_ns / 1000000.0,
                (double)elapsed_ns(swd_start, swd_end) / 1000000.0,
                (double)send_ns / 1000000.0,
                total_ns == 0U ? 0.0 :
                    ((double)count * 1000000000.0 / (double)total_ns) / 1024.0);
    }
    free(data);
    return success;
}

static bool handle_memory_write(struct gdb_target *target,
                                int fd,
                                const uint8_t *packet,
                                size_t length)
{
    const uint8_t *comma = memchr(packet, ',', length);
    const uint8_t *colon = memchr(packet, ':', length);
    if (comma == NULL || colon == NULL || comma <= packet || colon <= comma) {
        return send_error(fd, 1U);
    }
    uint32_t address = 0U;
    size_t count = 0U;
    if (!parse_hex_u32(packet + 1U,
                       (size_t)(comma - (packet + 1U)),
                       &address) ||
        !parse_hex_size(comma + 1U,
                        (size_t)(colon - (comma + 1U)),
                        &count) ||
        count > max_memory_request) {
        return send_error(fd, 1U);
    }
    uint8_t *data = malloc(count == 0U ? 1U : count);
    if (data == NULL || !decode_hex_bytes(colon + 1U,
                                          (size_t)(packet + length - (colon + 1U)),
                                          data,
                                          count)) {
        free(data);
        return send_error(fd, 1U);
    }
    struct timespec start = {0};
    struct timespec end = {0};
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    }
    /* Architectural reset writes from the browser need the same MEM-AP
     * recovery as monitor system_reset. Otherwise SYSRESETREQ can fault the
     * posted drain and poison every later request on this connection. */
    bool reset = address == UINT32_C(0xe000ed0c) && count == 4U &&
                 (load_le32(data) & UINT32_C(0xffff0004)) == UINT32_C(0x05fa0004);
    bool success = reset ? gpio_dap_reset(&target->dap)
                         : gpio_dap_write_memory(&target->dap, address, data, count);
    if (reset && success) target->running = false;
    invalidate_read_cache(target);
    if (target->debug) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
        uint64_t elapsed = elapsed_ns(start, end);
        target->stats.write_requests++;
        target->stats.write_bytes += count;
        target->stats.write_ns += elapsed;
        fprintf(stderr,
                "[gdb-debug] M addr=0x%08" PRIx32 " bytes=%zu time=%.3f ms rate=%.1f KiB/s\n",
                address, count, (double)elapsed / 1000000.0,
                elapsed == 0U ? 0.0 :
                    ((double)count * 1000000000.0 / (double)elapsed) / 1024.0);
    }
    free(data);
    return success ? send_text_packet(fd, "OK") : send_error(fd, 1U);
}

static bool register_index(const uint8_t *packet,
                           size_t length,
                           unsigned *index)
{
    uint32_t parsed = 0U;
    if (!parse_hex_u32(packet, length, &parsed)) {
        return false;
    }
    if (parsed == 25U) parsed = 16U;
    if (parsed < core_register_count) {
        *index = (unsigned)parsed;
        return true;
    }
    if (parsed >= 26U &&
        parsed < 26U + m_system_register_count) {
        *index = m_system_register_indices[parsed - 26U];
        return true;
    }
    return false;
}

static bool writable_register_index(const uint8_t *packet,
                                    size_t length,
                                    unsigned *index)
{
    uint32_t parsed = 0U;
    if (!parse_hex_u32(packet, length, &parsed)) {
        return false;
    }
    if (parsed == 25U) parsed = 16U;
    if (parsed >= core_register_count) return false;
    *index = (unsigned)parsed;
    return true;
}

static bool read_register(struct gdb_target *target,
                          unsigned index,
                          uint32_t *value)
{
    if (index < core_register_count) {
        return gpio_dap_read_core_register(&target->dap, index, value);
    }
    return gpio_dap_read_m_system_register(&target->dap, index, value);
}

static bool handle_register_read(struct gdb_target *target,
                                 int fd,
                                 const uint8_t *packet,
                                 size_t length)
{
    if (!gpio_dap_halt(&target->dap)) return send_error(fd, 1U);
    target->running = false;
    unsigned index = 0U;
    uint32_t value = 0U;
    uint8_t bytes[sizeof(value)];
    uint8_t response[sizeof(value) * 2U + 1U];
    if (!register_index(packet + 1U, length - 1U, &index) ||
        !read_register(target, index, &value)) {
        return send_error(fd, 1U);
    }
    store_le32(bytes, value);
    (void)encode_hex_bytes(bytes, sizeof(bytes), response);
    response[sizeof(response) - 1U] = '\0';
    return send_text_packet(fd, (const char *)response);
}

static bool handle_register_write(struct gdb_target *target,
                                  int fd,
                                  const uint8_t *packet,
                                  size_t length)
{
    if (!gpio_dap_halt(&target->dap)) return send_error(fd, 1U);
    target->running = false;
    const uint8_t *equals = memchr(packet, '=', length);
    unsigned index = 0U;
    uint8_t bytes[sizeof(uint32_t)] = {0U};
    if (equals == NULL || !writable_register_index(packet + 1U,
                                                   (size_t)(equals - (packet + 1U)),
                                                   &index) ||
        !decode_hex_bytes(equals + 1U,
                          (size_t)(packet + length - (equals + 1U)),
                          bytes,
                          sizeof(bytes)) ||
        !gpio_dap_write_core_register(&target->dap, index, load_le32(bytes))) {
        return send_error(fd, 1U);
    }
    return send_text_packet(fd, "OK");
}

static bool handle_all_registers(struct gdb_target *target, int fd)
{
    if (!gpio_dap_halt(&target->dap)) return send_error(fd, 1U);
    target->running = false;
    uint8_t response[gdb_register_count * sizeof(uint32_t) * 2U + 1U];
    for (unsigned register_number = 0U;
         register_number < gdb_register_count;
         ++register_number) {
        unsigned index = 0U;
        if (register_number < core_register_count) {
            index = register_number;
        } else {
            index = m_system_register_indices[register_number - core_register_count];
        }
        uint32_t value = 0U;
        if (!read_register(target, index, &value)) {
            return send_error(fd, 1U);
        }
        uint8_t bytes[sizeof(value)];
        store_le32(bytes, value);
        (void)encode_hex_bytes(bytes,
                               sizeof(bytes),
                               response + register_number * sizeof(bytes) * 2U);
    }
    response[sizeof(response) - 1U] = '\0';
    return send_text_packet(fd, (const char *)response);
}

static bool decode_monitor_command(const uint8_t *packet,
                                   size_t length,
                                   char *command,
                                   size_t capacity)
{
    if (length < 6U || memcmp(packet, "qRcmd,", 6U) != 0) {
        return false;
    }
    size_t encoded_length = length - 6U;
    if (encoded_length / 2U + 1U > capacity) {
        return false;
    }
    for (size_t index = 0U; index < encoded_length / 2U; ++index) {
        int high = hex_value(packet[6U + index * 2U]);
        int low = hex_value(packet[7U + index * 2U]);
        if (high < 0 || low < 0) {
            return false;
        }
        command[index] = (char)((high << 4) | low);
    }
    command[encoded_length / 2U] = '\0';
    return encoded_length % 2U == 0U;
}

static const char *feature_xml(const uint8_t *annex, size_t length)
{
    if (length == strlen("target.xml") &&
        memcmp(annex, "target.xml", length) == 0) {
        return target_xml;
    }
    if (length == strlen("arm-m-profile.xml") &&
        memcmp(annex, "arm-m-profile.xml", length) == 0) {
        return arm_m_profile_xml;
    }
    if (length == strlen("arm-m-system.xml") &&
        memcmp(annex, "arm-m-system.xml", length) == 0) {
        return arm_m_system_xml;
    }
    return NULL;
}

static bool handle_feature_read(int fd,
                                const uint8_t *packet,
                                size_t length)
{
    static const char prefix[] = "qXfer:features:read:";
    const size_t prefix_length = sizeof(prefix) - 1U;
    if (length < prefix_length ||
        memcmp(packet, prefix, prefix_length) != 0) {
        return false;
    }

    const uint8_t *annex = packet + prefix_length;
    const uint8_t *range = memchr(annex,
                                  ':',
                                  (size_t)(packet + length - annex));
    if (range == NULL) {
        return send_error(fd, 1U);
    }
    const char *xml = feature_xml(annex, (size_t)(range - annex));
    if (xml == NULL) {
        return send_text_packet(fd, "");
    }

    const uint8_t *start = range + 1U;
    const uint8_t *comma = memchr(start,
                                  ',',
                                  (size_t)(packet + length - start));
    uint32_t offset = 0U;
    uint32_t requested = 0U;
    if (comma == NULL ||
        !parse_hex_u32(start, (size_t)(comma - start), &offset) ||
        !parse_hex_u32(comma + 1U,
                       (size_t)(packet + length - comma - 1U),
                       &requested)) {
        return send_error(fd, 1U);
    }

    size_t total = strlen(xml);
    if ((uint64_t)offset >= (uint64_t)total) {
        return send_text_packet(fd, "l");
    }
    size_t count = total - (size_t)offset;
    if ((uint64_t)requested < (uint64_t)count) {
        count = (size_t)requested;
    }
    char *response = malloc(count + 2U);
    if (response == NULL) {
        return send_error(fd, 2U);
    }
    response[0] = (size_t)offset + count < total ? 'm' : 'l';
    memcpy(response + 1U, xml + offset, count);
    response[count + 1U] = '\0';
    bool success = send_text_packet(fd, response);
    free(response);
    return success;
}

static bool handle_packet(struct gdb_target *target,
                          int fd,
                          const uint8_t *packet,
                          size_t length)
{
    if (length == 0U) {
        return send_text_packet(fd, "");
    }
    switch (packet[0]) {
    case '?':
        return send_text_packet(fd, target->running ? "S05" : "S02");
    case 'm':
        return handle_memory_read(target, fd, packet, length);
    case 'x':
        return handle_memory_read_binary(target, fd, packet, length);
    case 'M':
        return handle_memory_write(target, fd, packet, length);
    case 'p':
        return handle_register_read(target, fd, packet, length);
    case 'P':
        return handle_register_write(target, fd, packet, length);
    case 'g':
        return handle_all_registers(target, fd);
    case 'c':
        bool resumed = gpio_dap_resume(&target->dap);
        if (!resumed) {
            return false;
        }
        target->running = true;
        /* receive_packet already sent the protocol acknowledgement. The
         * target remains running, so there is no stop-reply packet yet. This
         * matches GnWManager's GDB backend, which waits only for '+'. */
        return true;
    case 's':
        target->running = false;
        return send_text_packet(fd, "S05");
    case 'D':
        /* Standard GDB detach releases the target; disconnect merely closes
         * the socket and is intentionally different. */
        if (!gpio_dap_resume(&target->dap)) return send_error(fd, 1U);
        target->running = false;
        gpio_dap_detach(&target->dap);
        target->attached = false;
        return send_text_packet(fd, "OK");
    case 'H':
    case '!':
    case 'T':
    case 'Z':
    case 'z':
        return send_text_packet(fd, "OK");
    case 'q':
        if (length >= 10U && memcmp(packet, "qSupported", 10U) == 0) {
            return send_text_packet(fd, "PacketSize=10000;qXfer:features:read+");
        }
        if (length >= strlen("qXfer:features:read:") &&
            memcmp(packet,
                   "qXfer:features:read:",
                   strlen("qXfer:features:read:")) == 0) {
            return handle_feature_read(fd, packet, length);
        }
        if (length == 9U && memcmp(packet, "qAttached", 9U) == 0) {
            return send_text_packet(fd, "1");
        }
        if (length == 2U && memcmp(packet, "qC", 2U) == 0) {
            return send_text_packet(fd, "QC1");
        }
        if (length == 8U && memcmp(packet, "qTStatus", 8U) == 0) {
            return send_text_packet(fd, "T0;tnotrun:0");
        }
        if (length >= 6U && memcmp(packet, "qRcmd,", 6U) == 0) {
            char command[64];
            if (!decode_monitor_command(packet, length, command, sizeof(command))) {
                return send_error(fd, 1U);
            }
            if (strcmp(command, "system_reset") == 0 &&
                gpio_dap_reset(&target->dap)) {
                target->running = false;
                return send_text_packet(fd, "OK");
            }
            return send_error(fd, 1U);
        }
        return send_text_packet(fd, "");
    case 'Q':
        if (length >= 15U && memcmp(packet, "QStartNoAckMode", 15U) == 0) {
            return send_text_packet(fd, "OK");
        }
        return send_text_packet(fd, "");
    default:
        return send_text_packet(fd, "");
    }
}

static bool handle_client(struct gdb_target *target, int fd)
{
    if (target->debug) {
        memset(&target->stats, 0, sizeof(target->stats));
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &target->stats.started);
        fprintf(stderr, "[gdb-debug] client attached\n");
    }
    if (!target->attached) {
        if (!gpio_dap_attach(&target->dap, &target->idcode)) {
            (void)send_text_packet(fd, "E01");
            return false;
        }
        target->attached = true;
        target->running = false;
    }

    uint8_t packet[max_packet_size];
    for (;;) {
        size_t length = 0U;
        struct timespec packet_started = {0};
        struct timespec packet_received = {0};
        if (target->debug) {
            (void)clock_gettime(CLOCK_MONOTONIC_RAW, &packet_started);
        }
        enum packet_result result = receive_packet(fd,
                                                   packet,
                                                   sizeof(packet),
                                                   &length);
        if (target->debug) {
            (void)clock_gettime(CLOCK_MONOTONIC_RAW, &packet_received);
            target->stats.packet_started = packet_started;
            target->stats.packet_receive_ns = elapsed_ns(packet_started, packet_received);
        }
        if (result == packet_closed) {
            break;
        }
        if (result == packet_retry) {
            continue;
        }
        if (result == packet_interrupt) {
            bool halted = gpio_dap_halt(&target->dap);
            if (!halted) {
                return false;
            }
            target->running = false;
            if (!send_text_packet(fd, "S02")) {
                return false;
            }
            continue;
        }
        if (target->debug) {
            target->stats.packets++;
        }
        if (!handle_packet(target, fd, packet, length)) {
            return false;
        }
        if (length != 0U && packet[0] == 'D') {
            break;
        }
    }
    gpio_dap_detach(&target->dap);
    target->attached = false;
    if (target->debug) {
        struct timespec ended = {0};
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &ended);
        uint64_t elapsed = elapsed_ns(target->stats.started, ended);
        fprintf(stderr,
                "[gdb-debug] client closed elapsed=%.3f s packets=%" PRIu64
                " reads=%" PRIu64 "/%" PRIu64 " bytes windows=%" PRIu64
                " writes=%" PRIu64 "/%" PRIu64 " bytes\n",
                (double)elapsed / 1000000000.0,
                target->stats.packets,
                target->stats.read_requests,
                target->stats.read_bytes,
                target->stats.read_windows,
                target->stats.write_requests,
                target->stats.write_bytes);
        fprintf(stderr,
                "[gdb-debug] transfer_time read=%.3f s write=%.3f s\n",
                (double)target->stats.read_ns / 1000000000.0,
                (double)target->stats.write_ns / 1000000000.0);
        fprintf(stderr,
                "[gdb-debug] read_split receive=%.3f s swd=%.3f s encode=%.3f s send=%.3f s total=%.3f s\n",
                (double)target->stats.read_receive_ns / 1000000000.0,
                (double)target->stats.read_ns / 1000000000.0,
                (double)target->stats.read_encode_ns / 1000000000.0,
                (double)target->stats.read_send_ns / 1000000000.0,
                (double)target->stats.read_total_ns / 1000000000.0);
        if (target->read_cache_size != 0U) {
            fprintf(stderr,
                    "[gdb-debug] cache=%zu KiB hits=%" PRIu64 " misses=%" PRIu64 "\n",
                    target->read_cache_size / 1024U,
                    target->stats.cache_hits,
                    target->stats.cache_misses);
        }
    }
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

int main(int argc, char **argv)
{
    const char *bind_address = "127.0.0.1";
    unsigned port = default_port;
    unsigned swclk = default_swclk_pin;
    unsigned swdio = default_swdio_pin;
    unsigned clock_hz = default_clock_hz;
    bool debug = false;
    size_t cache_read_size = 0U;

    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc) {
            bind_address = argv[++index];
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], 1U, 65535U, &value)) {
            port = (unsigned)value;
        } else if (strcmp(argv[index], "--swclk") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], 0U, 53U, &value)) {
            swclk = (unsigned)value;
        } else if (strcmp(argv[index], "--swdio") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], 0U, 53U, &value)) {
            swdio = (unsigned)value;
        } else if (strcmp(argv[index], "--clock") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], 1U, UINT32_MAX, &value)) {
            clock_hz = (unsigned)value;
        } else if (strcmp(argv[index], "--debug") == 0) {
            debug = true;
        } else if (strcmp(argv[index], "--cache-read-kib") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], 1U, read_cache_capacity / 1024U, &value)) {
            cache_read_size = (size_t)value * 1024U;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    (void)signal(SIGPIPE, SIG_IGN);
    struct gdb_target target = {0};
    target.debug = debug;
    target.read_cache_size = cache_read_size;
    int result = gpio_dap_open(&target.dap, swclk, swdio, clock_hz);
    if (result != 0) {
        fprintf(stderr, "cannot open GPIO SWD backend: %s\n", strerror(result));
        return EXIT_FAILURE;
    }
    if (!gpio_dap_attach(&target.dap, &target.idcode)) {
        fprintf(stderr, "cannot attach to target over SWD\n");
        gpio_dap_close(&target.dap);
        return EXIT_FAILURE;
    }
    target.attached = true;

    int server = listen_socket(bind_address, port);
    if (server < 0) {
        perror("GDB SWD listen");
        gpio_dap_close(&target.dap);
        return EXIT_FAILURE;
    }
    fprintf(stderr,
            "GDB SWD listening on %s:%u, SWCLK GPIO%u, SWDIO GPIO%u, "
            "clock %u Hz, DP IDCODE 0x%08" PRIx32 "\n",
            bind_address,
            port,
            swclk,
            swdio,
            clock_hz,
            target.idcode);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("GDB SWD accept");
            break;
        }
        int no_delay = 1;
        (void)setsockopt(client,
                         IPPROTO_TCP,
                         TCP_NODELAY,
                         &no_delay,
                         sizeof(no_delay));
        (void)handle_client(&target, client);
        (void)close(client);
        /* Early command failures must also clear the faulted session. */
        gpio_dap_detach(&target.dap);
        target.attached = false;
        invalidate_read_cache(&target);
    }

    (void)close(server);
    gpio_dap_close(&target.dap);
    return EXIT_SUCCESS;
}
