#define _GNU_SOURCE

/*
 * SPI0 REN SWD memory reader.
 *
 * The SPI/SWD byte engine is deliberately shared with spi_swd_probe.c.  That
 * source is the known-good IDCODE path; keeping one implementation avoids
 * subtle differences in BCM2711 CS/REN sequencing.
 */
#define main spi_swd_probe_standalone_main
#include "spi_swd_probe.c"
#undef main

#include <time.h>

struct memory_context {
    volatile uint32_t *gpio;
    volatile uint32_t *spi;
    uint32_t fsel_gpio_input;
    uint32_t fsel_gpio_output;
    unsigned retry_limit;
};

static bool gpio_clock_sample(const struct memory_context *context)
{
    const uint32_t swclk_mask = UINT32_C(1) << 11U;
    const uint32_t swdio_mask = UINT32_C(1) << 10U;
    context->gpio[gpio_clr0] = swclk_mask;
    __sync_synchronize();
    for (unsigned delay = 0U; delay < 128U; ++delay) {
        __asm__ __volatile__("nop");
    }
    bool high = (context->gpio[gpio_lev0] & swdio_mask) != 0U;
    context->gpio[gpio_set0] = swclk_mask;
    __sync_synchronize();
    return high;
}

static void send_gpio_request(const struct memory_context *context,
                              uint8_t request)
{
    const uint32_t swclk_mask = UINT32_C(1) << 11U;
    const uint32_t swdio_mask = UINT32_C(1) << 10U;
    context->spi[spi_cs] = 0U;
    context->gpio[gpio_set0] = swdio_mask;
    context->gpio[gpio_fsel1] = context->fsel_gpio_output;
    __sync_synchronize();
    for (unsigned bit = 0U; bit < 8U; ++bit) {
        if ((request & (uint8_t)(1U << bit)) != 0U) {
            context->gpio[gpio_set0] = swdio_mask;
        } else {
            context->gpio[gpio_clr0] = swdio_mask;
        }
        context->gpio[gpio_clr0] = swclk_mask;
        __sync_synchronize();
        context->gpio[gpio_set0] = swclk_mask;
        __sync_synchronize();
    }
}

static bool receive_ack(const struct memory_context *context)
{
    /* A write response is only turnaround + 3 ACK bits. Receiving a whole
     * SPI byte would consume four clocks of the following data phase. */
    context->spi[spi_cs] = 0U;
    context->gpio[gpio_fsel1] = context->fsel_gpio_input;
    __sync_synchronize();
    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (gpio_clock_sample(context)) {
            ack |= 1U << bit;
        }
    }

    (void)gpio_clock_sample(context); /* target-to-host turnaround */
    context->gpio[gpio_set0] = UINT32_C(1) << 10U;
    context->gpio[gpio_fsel1] = context->fsel_gpio_output;
    __sync_synchronize();
    if (ack != 1U) {
        fprintf(stderr, "SPI REN write ACK=0x%x\n", ack);
        return false;
    }
    return true;
}

static bool send_write_payload(const struct memory_context *context,
                               uint32_t value)
{
    uint8_t payload[5] = {0};
    unsigned ones = 0U;
    unsigned bit = 0U;

    /* Host turnaround, 32 LSB-first data bits, parity, then idle high. */
    payload[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
    ++bit;
    for (unsigned data_bit = 0U; data_bit < 32U; ++data_bit, ++bit) {
        if (((value >> data_bit) & 1U) != 0U) {
            payload[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
            ++ones;
        }
    }
    if ((ones & 1U) != 0U) {
        payload[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
    }
    ++bit;
    while (bit < sizeof(payload) * 8U) {
        payload[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
        ++bit;
    }

    return send_output(context->spi, payload, sizeof(payload));
}

static bool transfer(const struct memory_context *context,
                     bool ap,
                     bool read,
                     uint8_t address,
                     uint32_t write_value,
                     uint32_t *read_value)
{
    uint8_t request = swd_request(ap, read, address);
    for (unsigned attempt = 0U; attempt < context->retry_limit; ++attempt) {
        send_gpio_request(context, request);

        uint32_t value = 0U;
        if (read) {
            uint8_t response[5] = {0};
            if (spi_read_response(context->spi, response, &value)) {
                if (read_value != NULL) {
                    *read_value = value;
                }
                return true;
            }
            fprintf(stderr, "SPI REN read failed ap=%u addr=0x%x attempt=%u\n",
                    ap ? 1U : 0U, address, attempt + 1U);
        } else if (receive_ack(context) &&
                   send_write_payload(context, write_value)) {
            return true;
        } else {
            fprintf(stderr, "SPI REN write failed ap=%u addr=0x%x attempt=%u\n",
                    ap ? 1U : 0U, address, attempt + 1U);
        }
    }
    return false;
}

static bool prepare_memory(const struct memory_context *context)
{
    uint32_t status = 0U;
    if (!transfer(context, false, false, 0U, 0x0000001eU, NULL)) {
        fprintf(stderr, "prepare: DP ABORT failed\n");
        return false;
    }
    if (!transfer(context, false, false, 4U, 0x50000000U, NULL)) {
        fprintf(stderr, "prepare: DP CTRL/STAT write failed\n");
        return false;
    }
    if (!transfer(context, false, true, 4U, 0U, &status)) {
        fprintf(stderr, "prepare: DP CTRL/STAT read failed\n");
        return false;
    }
    if ((status & UINT32_C(0xa0000000)) != UINT32_C(0xa0000000)) {
        fprintf(stderr, "prepare: DP CTRL/STAT=0x%08" PRIx32 "\n", status);
        return false;
    }
    if (!transfer(context, false, false, 8U, 0U, NULL)) {
        fprintf(stderr, "prepare: DP SELECT failed\n");
        return false;
    }
    if (!transfer(context, true, false, 0U, 0x23000052U, NULL)) {
        fprintf(stderr, "prepare: AP CSW failed\n");
        return false;
    }
    return true;
}

static bool read_memory(const struct memory_context *context,
                        uint32_t address,
                        uint32_t *words,
                        size_t word_count)
{
    uint32_t discard = 0U;
    if (!transfer(context, true, false, 4U, address, NULL) ||
        !transfer(context, true, true, 12U, 0U, &discard)) {
        return false;
    }

    for (size_t index = 0U; index < word_count; ++index) {
        bool last = index + 1U == word_count;
        if (!transfer(context, last ? false : true,
                      true, 12U, 0U, &words[index])) {
            return false;
        }
    }
    return true;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

int main(int argc, char **argv)
{
    uint32_t address = UINT32_C(0x24000000);
    size_t words = 256U;
    unsigned divider = 2U;
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
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else {
            fprintf(stderr,
                    "Usage: %s [--address HEX] [--words COUNT] "
                    "[--divider EVEN] [--output FILE]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    uint32_t *buffer = calloc(words, sizeof(uint32_t));
    if (buffer == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }

    int gpio_fd = -1;
    int spi_fd = -1;
    volatile uint32_t *gpio = map_registers(GPIO_BASE, &gpio_fd);
    volatile uint32_t *spi = map_registers(SPI0_BASE, &spi_fd);
    if (gpio == NULL || spi == NULL) {
        perror("map SPI/GPIO");
        if (spi != NULL) {
            unmap_registers(spi, spi_fd);
        }
        if (gpio != NULL) {
            unmap_registers(gpio, gpio_fd);
        }
        free(buffer);
        return EXIT_FAILURE;
    }

    uint32_t original_fsel = gpio[gpio_fsel1];
    uint32_t fsel_spi = original_fsel & ~((7U << 0U) | (7U << 3U));
    fsel_spi |= (4U << 0U) | (4U << 3U);
    gpio[gpio_fsel1] = fsel_spi;

    /* Exact initialization used by the proven spi_swd_probe --ren path. */
    spi[spi_cs] = cs_clear_tx | cs_clear_rx;
    spi[spi_clk] = divider;
    spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_ta;

    uint32_t fsel_gpio_input = original_fsel &
                               ~((7U << 0U) | (7U << 3U));
    fsel_gpio_input |= 1U << 3U; /* SWCLK output, SWDIO input. */
    uint32_t fsel_gpio_output = fsel_gpio_input | (1U << 0U);
    struct memory_context context = {
        .gpio = gpio,
        .spi = spi,
        .fsel_gpio_input = fsel_gpio_input,
        .fsel_gpio_output = fsel_gpio_output,
        .retry_limit = 3U,
    };
    uint32_t idcode = 0U;
    uint8_t id_response[5] = {0};
    bool success = send_output(spi,
                               (const uint8_t[]){0xffU, 0xffU, 0xffU,
                                                 0xffU, 0xffU, 0xffU, 0xffU},
                               7U) &&
                   send_output(spi, (const uint8_t[]){0x79U, 0xe7U}, 2U) &&
                   send_output(spi,
                               (const uint8_t[]){0xffU, 0xffU, 0xffU,
                                                 0xffU, 0xffU, 0xffU, 0xffU},
                               7U) &&
                   send_output(spi, (const uint8_t[]){0x00U}, 1U);
    uint8_t id_request = reverse_bits(swd_request(false, true, 0U));
    if (success) {
        success = send_output(spi, &id_request, 1U) &&
                  spi_read_response(spi, id_response, &idcode);
        if (success) {
            fprintf(stderr, "SPI REN IDCODE probe=0x%08" PRIx32 "\n", idcode);
        }
    }

    struct timespec start = {0};
    struct timespec end = {0};
    if (success) {
        success = prepare_memory(&context);
    }
    if (success) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
        success = read_memory(&context, address, buffer, words);
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    }

    spi[spi_cs] = 0U;
    gpio[gpio_fsel1] = original_fsel;
    unmap_registers(spi, spi_fd);
    unmap_registers(gpio, gpio_fd);

    if (!success) {
        free(buffer);
        return EXIT_FAILURE;
    }

    if (output_path != NULL) {
        FILE *output = fopen(output_path, "wb");
        if (output == NULL || fwrite(buffer, sizeof(uint32_t), words, output) != words) {
            fprintf(stderr, "cannot write output file '%s'\n", output_path);
            if (output != NULL) {
                (void)fclose(output);
            }
            free(buffer);
            return EXIT_FAILURE;
        }
        (void)fclose(output);
    }

    double seconds = elapsed_seconds(&start, &end);
    printf("SPI REN IDCODE: 0x%08" PRIx32 "\n", idcode);
    printf("read: %zu bytes in %.6f s (%.1f KiB/s), divider=%u\n",
           words * sizeof(uint32_t), seconds,
           ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
           divider);
    free(buffer);
    return EXIT_SUCCESS;
}
