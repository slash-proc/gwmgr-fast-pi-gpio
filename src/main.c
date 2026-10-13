#define _GNU_SOURCE

#include "gpio_mmio.h"
#include "swd.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --swclk PIN       GPIO for SWCLK (default: 11)\n"
            "  --swdio PIN       GPIO for SWDIO (default: 10)\n"
            "  --clock HZ        SWCLK target frequency (default: 1000000)\n"
            "  --fast-gpio       use the fused CPU GPIO SWD read loop\n"
            "  --no-switch       skip the JTAG-to-SWD activation sequence\n"
            "  --address HEX     target RAM address (default: 0x24000000)\n"
            "  --length BYTES    bytes to read, multiple of 4 (default: 262144)\n"
            "  --output FILE     save bytes instead of discarding them\n",
            program);
}

static bool parse_unsigned(const char *text, unsigned long *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static double elapsed_seconds(const struct timespec *start, const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

int main(int argc, char **argv)
{
    unsigned swclk = 11U;
    unsigned swdio = 10U;
    unsigned clock_hz = 1000000U;
    bool fast_gpio = false;
    bool activate = true;
    uint32_t address = 0x24000000U;
    size_t length = 256U * 1024U;
    const char *output_path = NULL;

    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--swclk") == 0 && index + 1 < argc && parse_unsigned(argv[++index], &value)) {
            swclk = (unsigned)value;
        } else if (strcmp(argv[index], "--swdio") == 0 && index + 1 < argc && parse_unsigned(argv[++index], &value)) {
            swdio = (unsigned)value;
        } else if (strcmp(argv[index], "--clock") == 0 && index + 1 < argc && parse_unsigned(argv[++index], &value)) {
            clock_hz = (unsigned)value;
        } else if (strcmp(argv[index], "--fast-gpio") == 0) {
            fast_gpio = true;
        } else if (strcmp(argv[index], "--no-switch") == 0) {
            activate = false;
        } else if (strcmp(argv[index], "--address") == 0 && index + 1 < argc && parse_unsigned(argv[++index], &value)) {
            address = (uint32_t)value;
        } else if (strcmp(argv[index], "--length") == 0 && index + 1 < argc && parse_unsigned(argv[++index], &value)) {
            length = (size_t)value;
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (length == 0U || length % 4U != 0U || clock_hz == 0U) {
        fprintf(stderr, "length must be non-zero and divisible by 4; clock must be non-zero\n");
        return EXIT_FAILURE;
    }

    uint8_t *buffer = malloc(length);
    if (buffer == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    /* Keep first-write page faults out of the timed SWD transfer. */
    memset(buffer, 0, length);

    struct gpio_mmio gpio = {0};
    int result = gpio_mmio_open(&gpio, swclk, swdio, clock_hz);
    if (result != 0) {
        fprintf(stderr, "cannot open GPIO: %s\n", strerror(result));
        free(buffer);
        return EXIT_FAILURE;
    }

    struct swd_probe probe = {
        .io = {
            .ctx = &gpio,
            .set_swdio_output = gpio_mmio_set_swdio_output,
            .write_swdio = gpio_mmio_write_swdio,
            .read_swdio = gpio_mmio_read_swdio,
            .write_swclk = gpio_mmio_write_swclk,
            .wait_half_cycle = gpio_mmio_wait_half_cycle,
            .clock_cycle = gpio_mmio_clock_cycle,
            .write_bit_cycle = gpio_mmio_write_bit_cycle,
            .read_bit_cycle = gpio_mmio_read_bit_cycle,
            .write_bits = gpio_mmio_write_bits,
            .read_bits = gpio_mmio_read_bits,
        },
        .retry_limit = 8U,
        .debug = false,
    };

    bool success = !activate || swd_switch_to_swd(&probe);
    uint32_t idcode = 0U;
    if (!success) {
        fprintf(stderr, "SWD activation or initial DP IDCODE read failed\n");
    }
    if (success) {
        success = swd_read_idcode(&probe, &idcode);
        if (!success) {
            fprintf(stderr, "DP IDCODE read failed after SWD activation\n");
        }
    }
    if (success) {
        success = swd_prepare_memory_access(&probe);
        if (!success) {
            fprintf(stderr, "DP/AP power-up or MEM-AP setup failed\n");
        }
    }

    struct timespec start = {0};
    struct timespec end = {0};
    size_t bytes_read = 0U;
    if (success) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
        if (fast_gpio) {
            success = gpio_mmio_fast_read_memory(&gpio,
                                                 address,
                                                 buffer,
                                                 length,
                                                 &bytes_read);
        } else {
            success = swd_read_memory(&probe,
                                      address,
                                      buffer,
                                      length,
                                      &bytes_read);
        }
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
        if (!success) {
            fprintf(stderr, "target memory read failed at 0x%08" PRIx32 "\n", address);
        }
    }

    if (success && output_path != NULL) {
        FILE *output = fopen(output_path, "wb");
        if (output == NULL || fwrite(buffer, 1U, bytes_read, output) != bytes_read) {
            fprintf(stderr, "cannot write output file '%s'\n", output_path);
            success = false;
        }
        if (output != NULL) {
            (void)fclose(output);
        }
    }

    if (success) {
        double seconds = elapsed_seconds(&start, &end);
        double kib_per_second = ((double)bytes_read / 1024.0) / seconds;
        printf("DP IDCODE: 0x%08" PRIx32 "\n", idcode);
        printf("read: %zu bytes in %.3f s (%.1f KiB/s), SWCLK requested=%u Hz\n",
               bytes_read,
               seconds,
               kib_per_second,
               clock_hz);
    } else {
        fprintf(stderr, "SWD operation failed\n");
    }

    gpio_mmio_close(&gpio);
    free(buffer);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
