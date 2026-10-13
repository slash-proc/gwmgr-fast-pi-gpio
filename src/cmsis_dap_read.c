#define _GNU_SOURCE

#include "cmsis_dap.h"
#include "gpio_dap.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

static void store_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static bool run_packet(struct cmsis_dap *dap,
                       const uint8_t *request,
                       size_t request_length,
                       uint8_t *response,
                       size_t *response_length)
{
    size_t length = cmsis_dap_process(dap,
                                       request,
                                       request_length,
                                       response,
                                       cmsis_dap_packet_size);
    if (length < 2U) {
        return false;
    }
    *response_length = length;
    return response[0] == request[0];
}

static bool transfer_setup(struct cmsis_dap *dap,
                           uint8_t *request,
                           uint8_t *response,
                           uint8_t transfer_request,
                           uint32_t value)
{
    request[0] = 0x05U;
    request[1] = 0U;
    request[2] = 1U;
    request[3] = transfer_request;
    if ((transfer_request & 0x02U) == 0U) {
        store_u32(request + 4U, value);
    }
    size_t response_length = 0U;
    return run_packet(dap,
                      request,
                      (transfer_request & 0x02U) == 0U ? 8U : 4U,
                      response,
                      &response_length) &&
           response_length >= 3U && response[1] == 1U && response[2] == 1U;
}

static bool transfer_read(struct cmsis_dap *dap,
                          uint8_t *request,
                          uint8_t *response,
                          uint8_t transfer_request,
                          uint32_t *value)
{
    request[0] = 0x05U;
    request[1] = 0U;
    request[2] = 1U;
    request[3] = transfer_request;
    size_t response_length = 0U;
    if (!run_packet(dap, request, 4U, response, &response_length) ||
        response_length != 7U || response[1] != 1U || response[2] != 1U) {
        return false;
    }
    *value = (uint32_t)response[3] |
             ((uint32_t)response[4] << 8U) |
             ((uint32_t)response[5] << 16U) |
             ((uint32_t)response[6] << 24U);
    return true;
}

int main(void)
{
    const uint32_t address = UINT32_C(0x24000000);
    const size_t length = 256U * 1024U;
    const uint16_t block_words =
        (uint16_t)((cmsis_dap_packet_size - 4U) / sizeof(uint32_t));
    uint8_t *memory = malloc(length);
    if (memory == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    struct gpio_dap_backend gpio_backend = {0};
    if (gpio_dap_open(&gpio_backend, 11U, 10U, 128000000U) != 0) {
        fprintf(stderr, "cannot open GPIO backend\n");
        free(memory);
        return EXIT_FAILURE;
    }
    struct cmsis_dap dap;
    struct cmsis_dap_backend backend = gpio_dap_make_backend(&gpio_backend);
    cmsis_dap_init(&dap, &backend);

    uint8_t request[cmsis_dap_packet_size] = {0};
    uint8_t response[cmsis_dap_packet_size] = {0};
    size_t response_length = 0U;
    request[0] = 0x02U;
    request[1] = 1U;
    if (!run_packet(&dap, request, 2U, response, &response_length) ||
        response_length != 2U || response[1] != 1U ||
        !transfer_setup(&dap, request, response, 0x00U, 0x0000001eU) ||
        !transfer_setup(&dap, request, response, 0x04U, 0x50000000U)) {
        fprintf(stderr, "CMSIS-DAP setup failed\n");
        gpio_dap_close(&gpio_backend);
        free(memory);
        return EXIT_FAILURE;
    }
    uint32_t ctrl_stat = 0U;
    bool powered = false;
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        if (!transfer_read(&dap, request, response, 0x06U, &ctrl_stat)) {
            break;
        }
        if ((ctrl_stat & UINT32_C(0xa0000000)) == UINT32_C(0xa0000000)) {
            powered = true;
            break;
        }
    }
    if (!powered ||
        !transfer_setup(&dap, request, response, 0x08U, 0U) ||
        !transfer_setup(&dap, request, response, 0x01U, 0x23000052U) ||
        !transfer_setup(&dap, request, response, 0x05U, address)) {
        fprintf(stderr, "CMSIS-DAP setup failed\n");
        gpio_dap_close(&gpio_backend);
        free(memory);
        return EXIT_FAILURE;
    }

    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    size_t offset = 0U;
    bool success = true;
    while (offset < length && success) {
        size_t words = (length - offset) / sizeof(uint32_t);
        if (words > block_words) {
            words = block_words;
        }
        request[0] = 0x06U;
        request[1] = 0U;
        request[2] = (uint8_t)words;
        request[3] = (uint8_t)(words >> 8U);
        request[4] = 0x0fU; /* AP read DRW */
        success = run_packet(&dap,
                             request,
                             5U,
                             response,
                             &response_length) &&
                  response_length == 4U + words * sizeof(uint32_t) &&
                  response[1] == (uint8_t)words &&
                  response[2] == (uint8_t)(words >> 8U) &&
                  response[3] == 1U;
        if (success) {
            memcpy(memory + offset, response + 4U, words * sizeof(uint32_t));
            offset += words * sizeof(uint32_t);
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);

    gpio_dap_close(&gpio_backend);
    if (!success) {
        fprintf(stderr, "CMSIS-DAP block read failed at byte %zu\n", offset);
        free(memory);
        return EXIT_FAILURE;
    }
    FILE *output = fopen("/tmp/cmsis-dap-read.bin", "wb");
    if (output == NULL || fwrite(memory, 1U, length, output) != length) {
        fprintf(stderr, "cannot write /tmp/cmsis-dap-read.bin\n");
        if (output != NULL) {
            (void)fclose(output);
        }
        free(memory);
        return EXIT_FAILURE;
    }
    (void)fclose(output);
    double seconds = elapsed_seconds(&start, &end);
    free(memory);
    printf("CMSIS-DAP TransferBlock read: %zu bytes in %.3f s (%.1f KiB/s)\n",
           length,
           seconds,
           ((double)length / 1024.0) / seconds);
    return EXIT_SUCCESS;
}
