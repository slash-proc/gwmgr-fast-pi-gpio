#define _GNU_SOURCE

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

static bool read_in_blocks(struct gpio_dap_backend *backend,
                           uint32_t address,
                           uint8_t *buffer,
                           size_t length,
                           size_t block_size,
                           double *seconds)
{
    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (size_t offset = 0U; offset < length; offset += block_size) {
        size_t count = length - offset;
        if (count > block_size) {
            count = block_size;
        }
        if (!gpio_dap_read_memory(backend,
                                   address + (uint32_t)offset,
                                   buffer + offset,
                                   count)) {
            return false;
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    *seconds = elapsed_seconds(&start, &end);
    return true;
}

int main(void)
{
    const uint32_t address = UINT32_C(0x24000000);
    const size_t length = 256U * 1024U;
    const size_t block_sizes[] = {4096U, 16384U, 65536U, 262144U};
    uint8_t *buffer = malloc(length);
    if (buffer == NULL) {
        return EXIT_FAILURE;
    }

    struct gpio_dap_backend backend = {0};
    if (gpio_dap_open(&backend, 11U, 10U, 128000000U) != 0) {
        free(buffer);
        return EXIT_FAILURE;
    }
    uint32_t idcode = 0U;
    if (!gpio_dap_attach(&backend, &idcode) || !gpio_dap_halt(&backend)) {
        fprintf(stderr, "GPIO DAP attach/halt failed\n");
        gpio_dap_close(&backend);
        free(buffer);
        return EXIT_FAILURE;
    }
    printf("DP IDCODE: 0x%08" PRIx32 "\n", idcode);
    for (size_t index = 0U;
         index < sizeof(block_sizes) / sizeof(block_sizes[0]);
         ++index) {
        double seconds = 0.0;
        bool success = read_in_blocks(&backend,
                                      address,
                                      buffer,
                                      length,
                                      block_sizes[index],
                                      &seconds);
        if (!success) {
            fprintf(stderr, "read failed for block size %zu\n",
                    block_sizes[index]);
            gpio_dap_close(&backend);
            free(buffer);
            return EXIT_FAILURE;
        }
        printf("block_size=%zu rate_kib_s=%.1f output=/tmp/gpio-dap-loop-%zu.bin\n",
               block_sizes[index],
               ((double)length / 1024.0) / seconds,
               block_sizes[index]);
        char path[80];
        (void)snprintf(path,
                       sizeof(path),
                       "/tmp/gpio-dap-loop-%zu.bin",
                       block_sizes[index]);
        FILE *output = fopen(path, "wb");
        if (output == NULL || fwrite(buffer, 1U, length, output) != length) {
            if (output != NULL) {
                (void)fclose(output);
            }
            gpio_dap_close(&backend);
            free(buffer);
            return EXIT_FAILURE;
        }
        (void)fclose(output);
    }
    gpio_dap_close(&backend);
    free(buffer);
    return EXIT_SUCCESS;
}
