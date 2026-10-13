#ifndef RPI_FAST_GPIO_CMSIS_DAP_H
#define RPI_FAST_GPIO_CMSIS_DAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CMSIS-DAPv2 uses the packet-size query to let a host select the largest
 * transfer block that fits its transport. The eventual USB HS FunctionFS
 * endpoint will advertise this same value. Keep the standard 1024-byte
 * default, while allowing larger TCP/native experiments at build time. */
#ifndef RPI_FAST_GPIO_CMSIS_DAP_PACKET_SIZE
#define RPI_FAST_GPIO_CMSIS_DAP_PACKET_SIZE 1024U
#endif

enum {
    cmsis_dap_packet_size = RPI_FAST_GPIO_CMSIS_DAP_PACKET_SIZE,
    cmsis_dap_packet_count = 1U,
};

struct cmsis_dap_backend {
    void *ctx;
    bool (*connect)(void *ctx, uint8_t port);
    void (*disconnect)(void *ctx);
    bool (*set_clock)(void *ctx, uint32_t clock_hz);
    bool (*swj_sequence)(void *ctx,
                         unsigned bit_count,
                         const uint8_t *bits);
    bool (*swd_configure)(void *ctx, const uint8_t *configuration);
    uint8_t (*swj_pins)(void *ctx,
                        uint8_t output,
                        uint8_t mask,
                        uint32_t delay_us);
    /* Return the CMSIS-DAP transfer response value: 1 for SWD ACK-OK,
     * 2 for WAIT, 4 for FAULT, or 7 for no response. */
    uint8_t (*transfer)(void *ctx, uint8_t request, uint32_t *data);
    uint8_t (*transfer_block)(void *ctx,
                              uint8_t request,
                              uint16_t count,
                              uint32_t *data);
};

struct cmsis_dap {
    struct cmsis_dap_backend backend;
    bool connected;
    uint8_t port;
    uint32_t clock_hz;
    uint8_t idle_cycles;
    uint16_t wait_retry;
    uint16_t match_retry;
    uint32_t block_words[cmsis_dap_packet_size / sizeof(uint32_t)];
};

void cmsis_dap_init(struct cmsis_dap *dap,
                    const struct cmsis_dap_backend *backend);

/* Process one complete CMSIS-DAP command packet. Returns the response length,
 * or zero for a malformed request or an output buffer that is too small. */
size_t cmsis_dap_process(struct cmsis_dap *dap,
                         const uint8_t *request,
                         size_t request_length,
                         uint8_t *response,
                         size_t response_capacity);

#endif
