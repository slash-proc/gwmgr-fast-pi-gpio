#define _GNU_SOURCE

/*
 * Fast read benchmark: use the validated GPIO SWD implementation to put the
 * MEM-AP at the requested address, then use SPI0's hardware clock for the
 * posted AP-read stream.  This deliberately has no SPI write path; SWD write
 * ACKs are not byte-aligned and are handled by the CPU setup phase.
 */

#include "gpio_mmio.h"
#include "swd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    page_size = 4096,
    gpio_fsel1 = 0x04 / sizeof(uint32_t),
    gpio_set0 = 0x1c / sizeof(uint32_t),
    spi_cs = 0x00 / sizeof(uint32_t),
    spi_fifo = 0x04 / sizeof(uint32_t),
    spi_clk = 0x08 / sizeof(uint32_t),
    cs_clear_tx = 1U << 4,
    cs_clear_rx = 1U << 5,
    cs_ta = 1U << 7,
    cs_done = 1U << 16,
    cs_rxd = 1U << 17,
    cs_txd = 1U << 18,
    cs_ren = 1U << 12,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define GPIO_BASE (PERIPHERAL_BASE + UINT32_C(0x00200000))
#define SPI0_BASE (PERIPHERAL_BASE + UINT32_C(0x00204000))
#define GPIO_SWCLK (UINT32_C(1) << 11U)
#define GPIO_SWDIO (UINT32_C(1) << 10U)

struct mapping {
    volatile uint32_t *regs;
    int fd;
};

struct spi_context {
    volatile uint32_t *gpio;
    volatile uint32_t *spi;
    uint32_t fsel_output;
    unsigned retry_limit;
};

static void spi_start_output(const struct spi_context *context);

static volatile uint32_t *map_registers(uint32_t address, int *fd_out)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        return NULL;
    }
    void *mapping = mmap(NULL,
                         page_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         fd,
                         address);
    if (mapping == MAP_FAILED) {
        (void)close(fd);
        return NULL;
    }
    *fd_out = fd;
    return (volatile uint32_t *)mapping;
}

static void unmap_registers(volatile uint32_t *mapping, int fd)
{
    (void)munmap((void *)mapping, page_size);
    (void)close(fd);
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

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

static uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U));
    value = (uint8_t)(((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U));
    return (uint8_t)((value << 4U) | (value >> 4U));
}

static bool wait_for(const struct spi_context *context, uint32_t mask)
{
    for (unsigned attempt = 0U; attempt < 10000000U; ++attempt) {
        if ((context->spi[spi_cs] & mask) != 0U) {
            return true;
        }
    }
    return false;
}

static bool spi_send_byte(const struct spi_context *context, uint8_t value)
{
    if (!wait_for(context, cs_txd)) {
        return false;
    }
    context->spi[spi_fifo] = value;
    if (!wait_for(context, cs_rxd)) {
        return false;
    }
    (void)context->spi[spi_fifo];
    return true;
}

static bool spi_receive_byte(const struct spi_context *context, uint8_t *value)
{
    if (!wait_for(context, cs_txd)) {
        return false;
    }
    context->spi[spi_fifo] = 0U;
    if (!wait_for(context, cs_rxd)) {
        return false;
    }
    *value = (uint8_t)context->spi[spi_fifo];
    return true;
}

static bool spi_send_sequence(const struct spi_context *context,
                              const uint8_t *bytes,
                              size_t count)
{
    spi_start_output(context);
    for (size_t index = 0U; index < count; ++index) {
        if (!spi_send_byte(context, bytes[index])) {
            return false;
        }
    }
    return true;
}

static void spi_start_output(const struct spi_context *context)
{
    context->spi[spi_cs] = 0U;
    (void)wait_for(context, cs_done);
    context->gpio[gpio_set0] = GPIO_SWCLK | GPIO_SWDIO;
    context->gpio[gpio_fsel1] = context->fsel_output;
    __sync_synchronize();
    /* DONE is documented as read-only, but the BCM2835 Linux driver writes
     * it while resetting the controller because otherwise a later transfer
     * can inherit a stale completion state. */
    context->spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_done | cs_ta;
}

static bool spi_read_response(const struct spi_context *context,
                              uint32_t *value)
{
    uint8_t raw[5] = {0};
    context->spi[spi_cs] = 0U;
    context->spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_done |
                           cs_ta | cs_ren;
    for (size_t index = 0U; index < sizeof(raw); ++index) {
        if (!spi_receive_byte(context, &raw[index])) {
            return false;
        }
    }

    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if ((raw[(1U + bit) / 8U] &
             (uint8_t)(1U << (7U - ((1U + bit) % 8U)))) != 0U) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr,
                "SPI burst ACK=0x%x raw=%02x%02x%02x%02x%02x\n",
                ack, raw[0], raw[1], raw[2], raw[3], raw[4]);
        return false;
    }

    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        size_t stream_bit = 4U + bit;
        if ((raw[stream_bit / 8U] &
             (uint8_t)(1U << (7U - (stream_bit % 8U)))) != 0U) {
            result |= UINT32_C(1) << bit;
            ones++;
        }
    }
    size_t parity_bit = 36U;
    bool parity = (raw[parity_bit / 8U] &
                   (uint8_t)(1U << (7U - (parity_bit % 8U)))) != 0U;
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr,
                "SPI burst parity failure raw=%02x%02x%02x%02x%02x\n",
                raw[0], raw[1], raw[2], raw[3], raw[4]);
        return false;
    }
    *value = result;
    return true;
}

static bool spi_read_request(const struct spi_context *context,
                             uint8_t request,
                             uint32_t *value)
{
    for (unsigned attempt = 0U; attempt < context->retry_limit; ++attempt) {
        spi_start_output(context);
        if (!spi_send_byte(context, 0xffU) ||
            !spi_send_byte(context, reverse_bits(request))) {
            return false;
        }
        if (spi_read_response(context, value)) {
            return true;
        }
    }
    return false;
}

static bool spi_burst_read(const struct spi_context *context,
                           uint32_t *words,
                           size_t word_count)
{
    uint32_t discard = 0U;
    uint8_t ap_read = swd_encode_request(
        (struct swd_request){.ap = true, .read = true, .address = 12U});
    uint8_t dp_read = swd_encode_request(
        (struct swd_request){.ap = false, .read = true, .address = 12U});
    if (!spi_read_request(context, ap_read, &discard)) {
        return false;
    }
    for (size_t index = 0U; index < word_count; ++index) {
        uint8_t request = index + 1U == word_count ? dp_read : ap_read;
        if (!spi_read_request(context, request, &words[index])) {
            return false;
        }
    }
    return true;
}

static bool prepare_target(unsigned clock_hz,
                           uint32_t address,
                           struct gpio_mmio *gpio)
{
    if (gpio_mmio_open(gpio, 11U, 10U, clock_hz) != 0) {
        return false;
    }
    struct swd_probe probe = {
        .io = {
            .ctx = gpio,
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
        .retry_limit = 100U,
        .debug = false,
    };
    bool success = swd_switch_to_swd(&probe) &&
                   swd_prepare_memory_access(&probe);
    if (success) {
        success = swd_transfer(&probe,
                               (struct swd_request){
                                   .ap = true, .read = false, .address = 4U},
                               address,
                               NULL) == SWD_ACK_OK;
    }
    if (success) {
        /* Exercise one complete CPU AP read before handing the pins to SPI.
         * This drains any posted response and confirms the MEM-AP is ready. */
        uint32_t warmup = 0U;
        success = swd_read_memory(&probe,
                                  address,
                                  &warmup,
                                  sizeof(warmup),
                                  NULL);
    }
    if (success) {
        success = swd_transfer(&probe,
                               (struct swd_request){
                                   .ap = true, .read = false, .address = 4U},
                               address,
                               NULL) == SWD_ACK_OK;
    }
    if (success) {
        gpio_mmio_set_swdio_output(gpio, true);
        gpio_mmio_write_swdio(gpio, true);
        for (unsigned count = 0U; count < 8U; ++count) {
            gpio_mmio_clock_cycle(gpio);
        }
    }
    if (!success) {
        gpio_mmio_close(gpio);
    }
    return success;
}

int main(int argc, char **argv)
{
    uint32_t address = UINT32_C(0x24000000);
    size_t words = 256U;
    unsigned divider = 100U;
    unsigned retry_limit = 100U;
    const char *output_path = NULL;
    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--address") == 0 && index + 1 < argc &&
            parse_unsigned(argv[++index], &value)) {
            address = (uint32_t)value;
        } else if (strcmp(argv[index], "--words") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) && value > 0UL) {
            words = (size_t)value;
        } else if (strcmp(argv[index], "--divider") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) && value > 0UL &&
                   value <= UINT16_MAX && (value & 1UL) == 0UL) {
            divider = (unsigned)value;
        } else if (strcmp(argv[index], "--retries") == 0 &&
                   index + 1 < argc && parse_unsigned(argv[++index], &value) &&
                   value > 0UL && value <= UINT_MAX) {
            retry_limit = (unsigned)value;
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else {
            fprintf(stderr,
                    "Usage: %s [--address HEX] [--words COUNT] "
                    "[--divider EVEN] [--retries COUNT] [--output FILE]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }

    uint32_t *words_buffer = calloc(words, sizeof(uint32_t));
    if (words_buffer == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    struct gpio_mmio gpio = {0};
    if (!prepare_target(6000000U, address, &gpio)) {
        fprintf(stderr, "CPU SWD setup failed\n");
        free(words_buffer);
        return EXIT_FAILURE;
    }

    int spi_fd = -1;
    volatile uint32_t *spi = map_registers(SPI0_BASE, &spi_fd);
    if (spi == NULL) {
        perror("map SPI/GPIO");
        gpio_mmio_close(&gpio);
        free(words_buffer);
        return EXIT_FAILURE;
    }

    volatile uint32_t *gpio_regs = (volatile uint32_t *)gpio.regs;
    uint32_t original_fsel = gpio_regs[gpio_fsel1];
    uint32_t fsel_base = original_fsel & ~((7U << 0U) | (7U << 3U));
    struct spi_context context = {
        .gpio = gpio_regs,
        .spi = spi,
        .fsel_output = fsel_base | (4U << 0U) | (4U << 3U),
        .retry_limit = retry_limit,
    };
    gpio_regs[gpio_set0] = GPIO_SWCLK | GPIO_SWDIO;
    gpio_regs[gpio_fsel1] = context.fsel_output;
    __sync_synchronize();
    spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_done;
    spi[spi_clk] = divider;
    spi_start_output(&context);

    uint32_t spi_idcode = 0U;
    static const uint8_t line_reset[] = {
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U};
    uint8_t dp_id_request = swd_encode_request(
        (struct swd_request){.ap = false, .read = true, .address = 0U});
    if (!spi_send_sequence(&context, line_reset, sizeof(line_reset)) ||
        !spi_read_request(&context, dp_id_request, &spi_idcode)) {
        fprintf(stderr, "SPI burst DP IDCODE read failed\n");
        spi[spi_cs] = 0U;
        gpio_regs[gpio_fsel1] = original_fsel;
        unmap_registers(spi, spi_fd);
        gpio_mmio_close(&gpio);
        free(words_buffer);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "SPI burst DP IDCODE=0x%08" PRIx32 "\n", spi_idcode);

    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    bool success = spi_burst_read(&context, words_buffer, words);
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    spi[spi_cs] = 0U;
    gpio_regs[gpio_fsel1] = original_fsel;
    unmap_registers(spi, spi_fd);
    gpio_mmio_close(&gpio);

    if (!success) {
        free(words_buffer);
        return EXIT_FAILURE;
    }
    if (output_path != NULL) {
        FILE *output = fopen(output_path, "wb");
        if (output == NULL ||
            fwrite(words_buffer, sizeof(uint32_t), words, output) != words) {
            fprintf(stderr, "cannot write output file '%s'\n", output_path);
            if (output != NULL) {
                (void)fclose(output);
            }
            free(words_buffer);
            return EXIT_FAILURE;
        }
        (void)fclose(output);
    }
    double seconds = elapsed_seconds(&start, &end);
    printf("SPI burst read: %zu bytes in %.6f s (%.1f KiB/s, divider=%u)\n",
           words * sizeof(uint32_t), seconds,
           ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
           divider);
    free(words_buffer);
    return EXIT_SUCCESS;
}
