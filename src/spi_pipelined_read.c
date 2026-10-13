#define _GNU_SOURCE

/*
 * Hybrid high-throughput experiment:
 *
 * CPU GPIO SWD performs activation, power-up, TAR setup, and the posted-read
 * prime.  SPI0 REN then emits only read requests and captures the long read
 * responses.  Keeping all SPI writes out of this path avoids the BCM2711
 * byte-granularity problem at the SWD write turnaround.
 */
#include "gpio_mmio.h"
#include "swd.h"

#define main spi_swd_probe_standalone_main
#include "spi_swd_probe.c"
#undef main

#include <time.h>

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

static bool cpu_prepare(struct gpio_mmio *gpio,
                        uint32_t address,
                        uint32_t *idcode)
{
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
        .retry_limit = 8U,
        .debug = false,
    };

    if (!swd_switch_to_swd(&probe)) {
        fprintf(stderr, "CPU SWD switch failed\n");
        return false;
    }
    if (!swd_read_idcode(&probe, idcode)) {
        fprintf(stderr, "CPU SWD IDCODE failed\n");
        return false;
    }
    if (!swd_prepare_memory_access(&probe)) {
        fprintf(stderr, "CPU SWD memory preparation failed, IDCODE=0x%08" PRIx32 "\n",
                *idcode);
        return false;
    }

    enum swd_ack ack = SWD_ACK_INVALID;
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        ack = swd_transfer(&probe,
                           (struct swd_request){.ap = true, .read = false, .address = 4U},
                           address,
                           NULL);
        if (ack != SWD_ACK_WAIT) {
            break;
        }
    }
    if (ack != SWD_ACK_OK) {
        fprintf(stderr, "CPU SWD TAR write failed\n");
        return false;
    }

    /* Prime the posted AP read. Its returned value is intentionally unused. */
    uint32_t discard = 0U;
    ack = SWD_ACK_INVALID;
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        ack = swd_transfer(&probe,
                           (struct swd_request){.ap = true, .read = true, .address = 12U},
                           0U,
                           &discard);
        if (ack != SWD_ACK_WAIT) {
            break;
        }
    }
    if (ack != SWD_ACK_OK) {
        fprintf(stderr, "CPU SWD DRW prime failed\n");
        return false;
    }
    return true;
}

static bool spi_read_response_batched(volatile uint32_t *spi,
                                      uint32_t *value)
{
    uint8_t response[5] = {0};
    if (!spi_set_receive(spi, true)) {
        return false;
    }

    /* Keep the SPI shifter running continuously across the 40-bit response. */
    for (size_t index = 0U; index < sizeof(response); ++index) {
        if (!wait_for(spi, cs_txd)) {
            return false;
        }
        spi[spi_fifo] = 0U;
    }
    for (size_t index = 0U; index < sizeof(response); ++index) {
        if (!wait_for(spi, cs_rxd)) {
            return false;
        }
        response[index] = (uint8_t)spi[spi_fifo];
    }

    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (response_bit(response, 1U + bit)) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr, "batched SPI ACK=0x%x raw=%02x%02x%02x%02x%02x\n",
                ack, response[0], response[1], response[2],
                response[3], response[4]);
        return false;
    }

    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (response_bit(response, 4U + bit)) {
            result |= UINT32_C(1) << bit;
            ++ones;
        }
    }
    bool parity = response_bit(response, 36U);
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr, "batched SPI parity failure\n");
        return false;
    }
    *value = result;
    return true;
}

static bool spi_read_word(volatile uint32_t *spi,
                          bool ap,
                          uint32_t *value)
{
    uint8_t request = reverse_bits(swd_encode_request((struct swd_request){
        .ap = ap, .read = true, .address = 12U}));
    return send_output(spi, &request, 1U) &&
           spi_read_response_batched(spi, value);
}

int main(int argc, char **argv)
{
    uint32_t address = UINT32_C(0x24000000);
    size_t words = 65536U;
    unsigned divider = 200U;
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

    struct gpio_mmio gpio = {0};
    int result = gpio_mmio_open(&gpio, 11U, 10U, 1000000U);
    if (result != 0) {
        fprintf(stderr, "cannot open GPIO: %s\n", strerror(result));
        free(buffer);
        return EXIT_FAILURE;
    }

    uint32_t idcode = 0U;
    bool success = cpu_prepare(&gpio, address, &idcode);
    if (!success) {
        fprintf(stderr, "CPU SWD preparation failed\n");
        gpio_mmio_close(&gpio);
        free(buffer);
        return EXIT_FAILURE;
    }

    int gpio_fd = -1;
    int spi_fd = -1;
    volatile uint32_t *gpio_regs = map_registers(GPIO_BASE, &gpio_fd);
    volatile uint32_t *spi = map_registers(SPI0_BASE, &spi_fd);
    if (gpio_regs == NULL || spi == NULL) {
        perror("map SPI/GPIO");
        if (spi != NULL) {
            unmap_registers(spi, spi_fd);
        }
        if (gpio_regs != NULL) {
            unmap_registers(gpio_regs, gpio_fd);
        }
        gpio_mmio_close(&gpio);
        free(buffer);
        return EXIT_FAILURE;
    }

    uint32_t original_fsel = gpio_regs[gpio_fsel1];
    uint32_t spi_fsel = original_fsel & ~((7U << 0U) | (7U << 3U));
    spi_fsel |= (4U << 0U) | (4U << 3U);
    gpio_regs[gpio_fsel1] = spi_fsel;
    spi[spi_cs] = cs_clear_tx | cs_clear_rx;
    spi[spi_clk] = divider;
    spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_ta;

    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    for (size_t index = 0U; index < words; ++index) {
        bool last = index + 1U == words;
        if (!spi_read_word(spi, !last, &buffer[index])) {
            fprintf(stderr, "SPI read failed at word %zu\n", index);
            success = false;
            break;
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);

    spi[spi_cs] = 0U;
    gpio_regs[gpio_fsel1] = original_fsel;
    unmap_registers(spi, spi_fd);
    unmap_registers(gpio_regs, gpio_fd);
    gpio_mmio_close(&gpio);

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
    printf("hybrid SPI IDCODE: 0x%08" PRIx32 "\n", idcode);
    printf("read: %zu bytes in %.6f s (%.1f KiB/s), divider=%u\n",
           words * sizeof(uint32_t), seconds,
           ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
           divider);
    free(buffer);
    return EXIT_SUCCESS;
}
