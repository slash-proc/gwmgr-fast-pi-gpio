#ifndef GWMGR_FAST_PI_GPIO_SWD_H
#define GWMGR_FAST_PI_GPIO_SWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct swd_io {
    void *ctx;
    void (*set_swdio_output)(void *ctx, bool output);
    void (*write_swdio)(void *ctx, bool high);
    bool (*read_swdio)(void *ctx);
    void (*write_swclk)(void *ctx, bool high);
    void (*wait_half_cycle)(void *ctx);
    void (*clock_cycle)(void *ctx);
    void (*write_bit_cycle)(void *ctx, bool bit);
    bool (*read_bit_cycle)(void *ctx);
    void (*write_bits)(void *ctx, uint32_t value, unsigned count);
    uint32_t (*read_bits)(void *ctx, unsigned count);
};

struct swd_probe {
    struct swd_io io;
    unsigned retry_limit;
    bool debug;
};

enum swd_ack {
    SWD_ACK_OK = 0x1,
    SWD_ACK_WAIT = 0x2,
    SWD_ACK_FAULT = 0x4,
    SWD_ACK_INVALID = 0x0
};

struct swd_request {
    bool ap;
    bool read;
    uint8_t address;
};

uint8_t swd_encode_request(struct swd_request request);
bool swd_request_parity_is_valid(uint8_t request_byte);

void swd_line_reset(struct swd_probe *probe);
bool swd_switch_to_swd(struct swd_probe *probe);

enum swd_ack swd_transfer(struct swd_probe *probe,
                          struct swd_request request,
                          uint32_t write_data,
                          uint32_t *read_data);

bool swd_read_idcode(struct swd_probe *probe, uint32_t *idcode);
bool swd_prepare_memory_access(struct swd_probe *probe);
bool swd_read_memory(struct swd_probe *probe,
                     uint32_t address,
                     void *buffer,
                     size_t length,
                     size_t *bytes_read);

#endif
