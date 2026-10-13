#include "cmsis_dap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_backend {
    unsigned connects;
    unsigned transfers;
    unsigned block_transfers;
    uint8_t last_request;
};

static bool fake_connect(void *ctx, uint8_t port)
{
    struct fake_backend *fake = ctx;
    fake->connects++;
    return port == 1U;
}

static uint8_t fake_transfer(void *ctx, uint8_t request, uint32_t *data)
{
    struct fake_backend *fake = ctx;
    fake->transfers++;
    fake->last_request = request;
    if ((request & 0x02U) != 0U) {
        *data = UINT32_C(0x24000000) + fake->transfers * 4U;
    }
    return 1U;
}

static uint8_t fake_block(void *ctx, uint8_t request, uint16_t count, uint32_t *data)
{
    struct fake_backend *fake = ctx;
    fake->block_transfers++;
    fake->last_request = request;
    if ((request & 0x02U) != 0U) {
        for (uint16_t index = 0U; index < count; ++index) {
            data[index] = UINT32_C(0x24000000) + (uint32_t)index * 4U;
        }
    }
    return 1U;
}

static int expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test failure: %s\n", message);
        return 1;
    }
    return 0;
}

int main(void)
{
    struct fake_backend fake = {0};
    struct cmsis_dap_backend backend = {
        .ctx = &fake,
        .connect = fake_connect,
        .transfer = fake_transfer,
        .transfer_block = fake_block,
    };
    struct cmsis_dap dap;
    cmsis_dap_init(&dap, &backend);

    uint8_t request[1024] = {0};
    uint8_t response[1024] = {0};
    request[0] = 0x00U;
    request[1] = 0xffU;
    size_t length = cmsis_dap_process(&dap, request, 2U, response, sizeof(response));
    if (expect(length == 4U && response[0] == 0x00U && response[1] == 2U &&
                   response[2] == 0x00U && response[3] == 0x04U,
               "DAP_Info packet size") != 0) {
        return 1;
    }

    request[0] = 0x02U;
    request[1] = 1U;
    length = cmsis_dap_process(&dap, request, 2U, response, sizeof(response));
    if (expect(length == 2U && response[1] == 1U && fake.connects == 1U,
               "DAP_Connect SWD") != 0) {
        return 1;
    }

    request[0] = 0x06U;
    request[1] = 0U;
    request[2] = 3U;
    request[3] = 0U;
    request[4] = 0x9fU; /* AP read DRW */
    length = cmsis_dap_process(&dap, request, 5U, response, sizeof(response));
    if (expect(length == 16U && response[0] == 0x06U &&
                   response[1] == 3U && response[2] == 0U &&
                   response[3] == 1U &&
                   response[4] == 0x00U && response[5] == 0x00U &&
                   response[6] == 0x00U && response[7] == 0x24U &&
                   fake.block_transfers == 1U,
               "DAP_TransferBlock read") != 0) {
        return 1;
    }

    request[0] = 0x05U;
    request[1] = 0U;
    request[2] = 2U;
    request[3] = 0x00U;
    request[4] = 0x44U;
    request[5] = 0x33U;
    request[6] = 0x22U;
    request[7] = 0x11U;
    request[8] = 0x02U;
    length = cmsis_dap_process(&dap, request, 9U, response, sizeof(response));
    if (expect(length == 7U && response[0] == 0x05U && response[1] == 2U &&
                   response[2] == 1U && response[3] == 0x08U &&
                   response[4] == 0x00U && response[5] == 0x00U &&
                   response[6] == 0x24U,
               "DAP_Transfer mixed read/write") != 0) {
        return 1;
    }

    request[0] = 0x06U;
    request[1] = 0U;
    request[2] = 2U;
    request[3] = 0U;
    request[4] = 0x01U; /* AP write */
    request[5] = 0x78U;
    request[6] = 0x56U;
    request[7] = 0x34U;
    request[8] = 0x12U;
    request[9] = 0xefU;
    request[10] = 0xcdU;
    request[11] = 0xabU;
    request[12] = 0x90U;
    length = cmsis_dap_process(&dap, request, 13U, response, sizeof(response));
    if (expect(length == 4U && response[0] == 0x06U && response[1] == 2U &&
                   response[2] == 0U && response[3] == 1U,
               "DAP_TransferBlock write") != 0) {
        return 1;
    }

    puts("CMSIS-DAP tests passed");
    return 0;
}
