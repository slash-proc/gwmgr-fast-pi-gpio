#include "swd.h"

#include <stdbool.h>
#include <stdio.h>

static int expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test failure: %s\n", message);
        return 1;
    }
    return 0;
}

struct read_fault {
    unsigned requests;
    unsigned reads;
    unsigned parity_reads;
};

static void fake_direction(void *ctx, bool output) { (void)ctx; (void)output; }
static void fake_clock(void *ctx) { (void)ctx; }
static void fake_write(void *ctx, uint32_t value, unsigned count)
{
    (void)value;
    if (count == 8U) ((struct read_fault *)ctx)->requests++;
}
static void fake_bit_write(void *ctx, bool value) { (void)ctx; (void)value; }
static uint32_t fake_read(void *ctx, unsigned count)
{
    struct read_fault *fault = ctx;
    if (count == 3U) return SWD_ACK_OK;
    fault->reads++;
    return fault->reads == 1U ? 0U : 0x11223344U;
}
static bool fake_parity(void *ctx)
{
    struct read_fault *fault = ctx;
    /* Prime succeeds, then corrupt parity on an accepted posted AP read. */
    return ++fault->parity_reads == 2U;
}

static int test_posted_read_parity(void)
{
    struct read_fault fault = {0};
    struct swd_probe probe = {
        .io = {.ctx = &fault, .set_swdio_output = fake_direction,
               .write_swdio = fake_bit_write,
               .clock_cycle = fake_clock, .wait_half_cycle = fake_clock,
               .write_bits = fake_write, .read_bits = fake_read,
               .read_bit_cycle = fake_parity, .write_bit_cycle = fake_bit_write},
        .retry_limit = 3U,
    };
    uint32_t words[2] = {0};
    size_t completed = 99U;
    bool success = swd_read_memory(&probe, 0x240e6800U, words, sizeof(words), &completed);
    return expect(!success && completed == 0U && fault.requests == 3U,
                  "bad AP data parity must fail the block without advancing the pipeline again");
}

int main(void)
{
    struct swd_request dp_idcode = {.ap = false, .read = true, .address = 0U};
    struct swd_request ap_drw_read = {.ap = true, .read = true, .address = 12U};
    struct swd_request ap_tar_write = {.ap = true, .read = false, .address = 4U};

    if (expect(swd_encode_request(dp_idcode) == 0xa5U, "DP IDCODE request") != 0 ||
        expect(swd_encode_request(ap_drw_read) == 0x9fU, "AP DRW request") != 0 ||
        expect(swd_encode_request(ap_tar_write) == 0x8bU, "AP TAR request") != 0 ||
        expect(swd_request_parity_is_valid(swd_encode_request(dp_idcode)), "request parity") != 0 ||
        expect(swd_request_parity_is_valid(swd_encode_request(ap_drw_read)), "AP request parity") != 0 ||
        expect(!swd_request_parity_is_valid((uint8_t)(swd_encode_request(dp_idcode) ^ 0x20U)),
               "bad request parity") != 0) {
        return 1;
    }

    if (test_posted_read_parity() != 0) return 1;
    puts("SWD protocol tests passed");
    return 0;
}
