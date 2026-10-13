#include "cmsis_dap.h"

#include <string.h>

enum {
    dap_info = 0x00,
    dap_host_status = 0x01,
    dap_connect = 0x02,
    dap_disconnect = 0x03,
    dap_transfer_configure = 0x04,
    dap_transfer = 0x05,
    dap_transfer_block = 0x06,
    dap_transfer_abort = 0x07,
    dap_reset_target = 0x0a,
    dap_swj_pins = 0x10,
    dap_swj_clock = 0x11,
    dap_swj_sequence = 0x12,
    dap_swd_configure = 0x13,
};

enum {
    dap_ok = 0x00,
    dap_error = 0xff,
};

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

static size_t response_status(uint8_t command,
                              uint8_t status,
                              uint8_t *response,
                              size_t capacity)
{
    if (capacity < 2U) {
        return 0U;
    }
    response[0] = command;
    response[1] = status;
    return 2U;
}

static size_t response_string(uint8_t command,
                              const char *value,
                              uint8_t *response,
                              size_t capacity)
{
    size_t length = strlen(value) + 1U;
    if (length > UINT8_MAX || capacity < 2U + length) {
        return 0U;
    }
    response[0] = command;
    response[1] = (uint8_t)length;
    memcpy(response + 2U, value, length);
    return 2U + length;
}

static size_t process_info(const uint8_t *request,
                           size_t request_length,
                           uint8_t *response,
                           size_t capacity)
{
    if (request_length != 2U || capacity < 3U) {
        return 0U;
    }
    switch (request[1]) {
    case 0x01U:
        return response_string(dap_info, "rpi-fast-gpio", response, capacity);
    case 0x02U:
        return response_string(dap_info, "Raspberry Pi SWD Probe", response, capacity);
    case 0x03U:
        return response_string(dap_info, "rpi-fast-gpio-0001", response, capacity);
    case 0x04U:
        return response_string(dap_info, "2.1.0", response, capacity);
    case 0xf0U:
        if (capacity < 3U) {
            return 0U;
        }
        response[0] = dap_info;
        response[1] = 1U;
        response[2] = 1U; /* SWD only. */
        return 3U;
    case 0xfeU:
        response[0] = dap_info;
        response[1] = 1U;
        response[2] = cmsis_dap_packet_count;
        return 3U;
    case 0xffU:
        response[0] = dap_info;
        response[1] = 2U;
        store_u16(response + 2U, cmsis_dap_packet_size);
        return 4U;
    default:
        response[0] = dap_info;
        response[1] = 0U;
        return 2U;
    }
}

static uint8_t backend_transfer(struct cmsis_dap *dap,
                                uint8_t request,
                                uint32_t *data)
{
    if (!dap->connected || dap->backend.transfer == NULL) {
        return 7U;
    }
    return dap->backend.transfer(dap->backend.ctx, request, data);
}

static size_t process_transfer(struct cmsis_dap *dap,
                               const uint8_t *request,
                               size_t request_length,
                               uint8_t *response,
                               size_t capacity)
{
    if (request_length < 3U || capacity < 3U) {
        return 0U;
    }
    uint8_t count = request[2];
    size_t input_offset = 3U;
    size_t output_offset = 3U;
    if (capacity < 3U + (size_t)count * sizeof(uint32_t)) {
        return 0U;
    }
    response[0] = dap_transfer;
    response[1] = 0U;
    response[2] = 7U;
    for (uint8_t index = 0U; index < count; ++index) {
        if (input_offset >= request_length) {
            return 0U;
        }
        uint8_t transfer_request = request[input_offset++];
        bool read = (transfer_request & 0x02U) != 0U;
        uint32_t data = 0U;
        if (!read) {
            if (request_length - input_offset < sizeof(uint32_t)) {
                return 0U;
            }
            data = load_u32(request + input_offset);
            input_offset += sizeof(uint32_t);
        }
        uint8_t status = backend_transfer(dap, transfer_request, &data);
        response[2] = status;
        if (status != 1U) {
            break;
        }
        if (read) {
            store_u32(response + output_offset, data);
            output_offset += sizeof(uint32_t);
        }
        response[1]++;
    }
    if (response[1] == count) {
        response[2] = 1U;
    }
    return output_offset;
}

static size_t process_transfer_block(struct cmsis_dap *dap,
                                     const uint8_t *request,
                                     size_t request_length,
                                     uint8_t *response,
                                     size_t capacity)
{
    if (request_length < 5U || capacity < 4U) {
        return 0U;
    }
    uint16_t count = load_u16(request + 2U);
    uint8_t transfer_request = request[4];
    bool read = (transfer_request & 0x02U) != 0U;
    size_t data_bytes = (size_t)count * sizeof(uint32_t);
    size_t input_offset = 5U;
    size_t response_length = 4U + (read ? data_bytes : 0U);
    if (capacity < response_length ||
        (!read && request_length - input_offset < data_bytes)) {
        return 0U;
    }

    response[0] = dap_transfer_block;
    store_u16(response + 1U, 0U);
    response[3] = 7U;
    bool success = dap->connected;
    if (success && dap->backend.transfer_block != NULL) {
        if (!read) {
            for (uint16_t index = 0U; index < count; ++index) {
                dap->block_words[index] =
                    load_u32(request + input_offset +
                             (size_t)index * sizeof(uint32_t));
            }
        }
        uint8_t status = dap->backend.transfer_block(dap->backend.ctx,
                                                     transfer_request,
                                                     count,
                                                     dap->block_words);
        response[3] = status;
        success = status == 1U;
        if (success) {
            store_u16(response + 1U, count);
            if (read) {
                for (uint16_t index = 0U; index < count; ++index) {
                    store_u32(response + 4U + (size_t)index * sizeof(uint32_t),
                              dap->block_words[index]);
                }
            }
        }
    } else {
        for (uint16_t index = 0U; index < count; ++index) {
            uint32_t data = read
                                ? 0U
                                : load_u32(request + input_offset +
                                           (size_t)index * sizeof(uint32_t));
            uint8_t status = success
                                 ? backend_transfer(dap, transfer_request, &data)
                                 : 7U;
            response[3] = status;
            if (status != 1U) {
                success = false;
                break;
            }
            if (read) {
                store_u32(response + 4U + (size_t)index * sizeof(uint32_t), data);
            }
            store_u16(response + 1U, (uint16_t)(index + 1U));
        }
    }
    return response_length;
}

void cmsis_dap_init(struct cmsis_dap *dap,
                    const struct cmsis_dap_backend *backend)
{
    memset(dap, 0, sizeof(*dap));
    if (backend != NULL) {
        dap->backend = *backend;
    }
    dap->clock_hz = 1000000U;
    dap->wait_retry = 100U;
    dap->match_retry = 0U;
}

size_t cmsis_dap_process(struct cmsis_dap *dap,
                         const uint8_t *request,
                         size_t request_length,
                         uint8_t *response,
                         size_t response_capacity)
{
    if (dap == NULL || request == NULL || request_length == 0U ||
        response == NULL || response_capacity == 0U) {
        return 0U;
    }
    switch (request[0]) {
    case dap_info:
        return process_info(request, request_length, response, response_capacity);
    case dap_host_status:
        return request_length == 3U
                   ? response_status(dap_host_status, dap_ok, response, response_capacity)
                   : 0U;
    case dap_connect: {
        if (request_length != 2U || response_capacity < 2U) {
            return 0U;
        }
        uint8_t port = request[1] == 0U ? 1U : request[1];
        bool success = port == 1U && dap->backend.connect != NULL &&
                       dap->backend.connect(dap->backend.ctx, port);
        dap->connected = success;
        dap->port = success ? port : 0U;
        response[0] = dap_connect;
        response[1] = dap->port;
        return 2U;
    }
    case dap_disconnect:
        if (request_length != 1U) {
            return 0U;
        }
        if (dap->connected && dap->backend.disconnect != NULL) {
            dap->backend.disconnect(dap->backend.ctx);
        }
        dap->connected = false;
        dap->port = 0U;
        return response_status(dap_disconnect, dap_ok, response, response_capacity);
    case dap_transfer_configure:
        if (request_length != 6U) {
            return 0U;
        }
        dap->idle_cycles = request[1];
        dap->wait_retry = load_u16(request + 2U);
        dap->match_retry = load_u16(request + 4U);
        return response_status(dap_transfer_configure, dap_ok, response, response_capacity);
    case dap_swj_pins: {
        if (request_length != 7U || response_capacity < 2U) {
            return 0U;
        }
        uint8_t pins = request[1];
        if (dap->backend.swj_pins != NULL) {
            pins = dap->backend.swj_pins(dap->backend.ctx,
                                         request[1],
                                         request[2],
                                         load_u32(request + 3U));
        }
        response[0] = dap_swj_pins;
        response[1] = pins;
        return 2U;
    }
    case dap_transfer:
        return process_transfer(dap, request, request_length, response, response_capacity);
    case dap_transfer_block:
        return process_transfer_block(dap, request, request_length, response, response_capacity);
    case dap_transfer_abort:
        return request_length == 1U
                   ? response_status(dap_transfer_abort, dap_ok, response, response_capacity)
                   : 0U;
    case dap_reset_target:
        return request_length == 1U
                   ? response_status(dap_reset_target, dap_ok, response, response_capacity)
                   : 0U;
    case dap_swj_sequence: {
        if (request_length < 2U) {
            return 0U;
        }
        unsigned bit_count = request[1] == 0U ? 256U : (unsigned)request[1];
        size_t byte_count = ((size_t)bit_count + 7U) / 8U;
        if (request_length != 2U + byte_count) {
            return 0U;
        }
        bool success = dap->backend.swj_sequence == NULL ||
                       dap->backend.swj_sequence(dap->backend.ctx,
                                                  bit_count,
                                                  request + 2U);
        return response_status(dap_swj_sequence,
                               success ? dap_ok : dap_error,
                               response,
                               response_capacity);
    }
    case dap_swd_configure: {
        if (request_length != 2U || response_capacity < 2U) {
            return 0U;
        }
        bool success = dap->backend.swd_configure == NULL ||
                       dap->backend.swd_configure(dap->backend.ctx,
                                                  request + 1U);
        return response_status(dap_swd_configure,
                               success ? dap_ok : dap_error,
                               response,
                               response_capacity);
    }
    case dap_swj_clock: {
        if (request_length != 5U) {
            return 0U;
        }
        uint32_t clock_hz = load_u32(request + 1U);
        bool success = clock_hz != 0U &&
                       (dap->backend.set_clock == NULL ||
                        dap->backend.set_clock(dap->backend.ctx, clock_hz));
        if (success) {
            dap->clock_hz = clock_hz;
        }
        return response_status(dap_swj_clock,
                               success ? dap_ok : dap_error,
                               response,
                               response_capacity);
    }
    default:
        response[0] = 0xffU;
        return 1U;
    }
}
