#define _GNU_SOURCE

/* Test the ADIv5 MEM-AP packed-transfer path.
 *
 * With CSW.AddrInc=packed, a byte-sized DRW read performs four adjacent byte
 * accesses and returns them in one AP response.  A halfword-sized read does
 * the same for two halfwords.  The wire response remains one 32-bit word, so
 * the existing fused SWD reader can be reused after changing CSW.
 */

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

enum {
    csw_base = 0x23000000U,
    csw_prot = 0x02U,
    csw_addrinc_packed = 0x20U,
    csw_size_byte = 0x00U,
    csw_size_halfword = 0x01U,
    csw_size_word = 0x02U,
};

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

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

static bool setup_packed_csw(struct swd_probe *probe,
                             unsigned size,
                             uint32_t address)
{
    uint32_t csw = csw_base | csw_prot | csw_addrinc_packed | size;
    return swd_transfer(probe,
                        (struct swd_request){
                            .ap = true, .read = false, .address = 0U},
                        csw,
                        NULL) == SWD_ACK_OK &&
           swd_transfer(probe,
                        (struct swd_request){
                            .ap = true, .read = false, .address = 4U},
                        address,
                        NULL) == SWD_ACK_OK;
}

int main(int argc, char **argv)
{
    unsigned clock_hz = 128000000U;
    uint32_t address = UINT32_C(0x24000000);
    size_t length = 256U * 1024U;
    const char *mode = "byte";
    const char *output_path = NULL;

    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--clock") == 0 && index + 1 < argc &&
            parse_unsigned(argv[++index], &value) && value > 0UL &&
            value <= UINT32_MAX) {
            clock_hz = (unsigned)value;
        } else if (strcmp(argv[index], "--address") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value)) {
            address = (uint32_t)value;
        } else if (strcmp(argv[index], "--length") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) && value > 0UL &&
                   value % 4UL == 0UL) {
            length = (size_t)value;
        } else if (strcmp(argv[index], "--mode") == 0 && index + 1 < argc) {
            mode = argv[++index];
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else {
            fprintf(stderr,
                    "Usage: %s [--mode byte|half|word] [--clock HZ] "
                    "[--address HEX] [--length BYTES] [--output FILE]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }

    unsigned csw_size = csw_size_byte;
    if (strcmp(mode, "half") == 0) {
        csw_size = csw_size_halfword;
    } else if (strcmp(mode, "word") == 0) {
        csw_size = csw_size_word;
    } else if (strcmp(mode, "byte") != 0) {
        fprintf(stderr, "mode must be byte, half, or word\n");
        return EXIT_FAILURE;
    }

    uint8_t *buffer = malloc(length);
    if (buffer == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    memset(buffer, 0, length);

    struct gpio_mmio gpio = {0};
    if (gpio_mmio_open(&gpio, 11U, 10U, clock_hz) != 0) {
        fprintf(stderr, "cannot open GPIO\n");
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

    uint32_t idcode = 0U;
    bool success = swd_switch_to_swd(&probe) &&
                   swd_read_idcode(&probe, &idcode) &&
                   swd_prepare_memory_access(&probe) &&
                   setup_packed_csw(&probe, csw_size, address);

    struct timespec start = {0};
    struct timespec end = {0};
    size_t bytes_read = 0U;
    if (success) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
        success = gpio_mmio_fast_read_block(&gpio,
                                            buffer,
                                            length,
                                            &bytes_read);
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
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
        printf("packed mode=%s CSW=0x%08" PRIx32
               " IDCODE=0x%08" PRIx32 " read=%zu bytes in %.6f s (%.1f KiB/s)\n",
               mode,
               csw_base | csw_prot | csw_addrinc_packed | csw_size,
               idcode,
               bytes_read,
               seconds,
               ((double)bytes_read / 1024.0) / seconds);
    } else {
        fprintf(stderr, "packed transfer failed (mode=%s)\n", mode);
    }

    gpio_mmio_close(&gpio);
    free(buffer);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
