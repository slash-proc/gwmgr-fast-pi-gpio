#define _GNU_SOURCE

/* Batched kernel-SPI 3-wire SWD memory reader.
 *
 * Each SWD read is represented by a TX-only transfer containing an idle byte
 * and the request, followed by an RX-only transfer containing the turnaround,
 * ACK, data and parity bits.  SPI_3WIRE makes the bcm2835 driver toggle the
 * shared GPIO10/MOSI pin between those two directions.  The transfers are
 * submitted in chunks so the CPU is not involved once the SPI message is
 * queued.
 */

#include "gpio_mmio.h"
#include "swd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum {
    gpio_fsel1 = 0x04 / sizeof(uint32_t),
    gpio_set0 = 0x1c / sizeof(uint32_t),
    gpio_clr0 = 0x28 / sizeof(uint32_t),
    spi_chunk_words = 64U,
};

enum sync_mode {
    sync_none,
    sync_idle1,
    sync_idle7,
    sync_line_reset,
    sync_swj,
    sync_swj_reset,
};

static uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U));
    value = (uint8_t)(((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U));
    return (uint8_t)((value << 4U) | (value >> 4U));
}

static bool response_bit(const uint8_t response[5], unsigned bit)
{
    return (response[bit / 8U] &
            (uint8_t)(1U << (7U - (bit % 8U)))) != 0U;
}

static bool decode_response(const uint8_t response[5], uint32_t *value)
{
    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (response_bit(response, 1U + bit)) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr,
                "SPI 3-wire burst ACK=0x%x raw=%02x%02x%02x%02x%02x\n",
                ack,
                response[0], response[1], response[2],
                response[3], response[4]);
        return false;
    }

    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (response_bit(response, 4U + bit)) {
            result |= UINT32_C(1) << bit;
            ones++;
        }
    }
    bool parity = response_bit(response, 36U);
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr, "SPI 3-wire burst parity failure\n");
        return false;
    }
    *value = result;
    return true;
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

static const char *sync_mode_name(enum sync_mode mode)
{
    switch (mode) {
    case sync_none:
        return "none";
    case sync_idle1:
        return "idle1";
    case sync_idle7:
        return "idle7";
    case sync_line_reset:
        return "line-reset";
    case sync_swj:
        return "swj";
    case sync_swj_reset:
        return "swj-reset";
    }
    return "unknown";
}

static bool parse_sync_mode(const char *text, enum sync_mode *mode)
{
    static const struct {
        const char *name;
        enum sync_mode mode;
    } modes[] = {
        {"none", sync_none},
        {"idle1", sync_idle1},
        {"idle7", sync_idle7},
        {"line-reset", sync_line_reset},
        {"swj", sync_swj},
        {"swj-reset", sync_swj_reset},
    };
    for (size_t index = 0U; index < sizeof(modes) / sizeof(modes[0]);
         ++index) {
        if (strcmp(text, modes[index].name) == 0) {
            *mode = modes[index].mode;
            return true;
        }
    }
    return false;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
}

static bool prepare_target(uint32_t address, struct gpio_mmio *gpio)
{
    if (gpio_mmio_open(gpio, 11U, 10U, 6000000U) != 0) {
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
        /* Drain one posted AP response and restore TAR before handing the
         * pins to SPI. This mirrors the validated CPU baseline. */
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

static bool configure_spi(int fd, uint32_t speed_hz)
{
    uint32_t mode = SPI_NO_CS | SPI_3WIRE;
    uint8_t bits = 8U;
    return ioctl(fd, SPI_IOC_WR_MODE32, &mode) >= 0 &&
           ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) >= 0 &&
           ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) >= 0;
}

static size_t make_sync_prefix(enum sync_mode mode, uint8_t *prefix)
{
    size_t length = 0U;
    size_t idle_bytes = 0U;
    if (mode == sync_idle1) {
        idle_bytes = 1U;
    } else if (mode == sync_idle7 || mode == sync_line_reset ||
               mode == sync_swj || mode == sync_swj_reset) {
        idle_bytes = 7U;
    }
    for (size_t index = 0U; index < idle_bytes; ++index) {
        prefix[length++] = 0xffU;
    }
    if (mode == sync_line_reset) {
        prefix[length++] = 0x00U;
    } else if (mode == sync_swj || mode == sync_swj_reset) {
        prefix[length++] = 0x79U;
        prefix[length++] = 0xe7U;
        prefix[length++] = 0x00U;
    }
    if (mode == sync_swj_reset) {
        for (size_t index = 0U; index < 7U; ++index) {
            prefix[length++] = 0xffU;
        }
    }
    return length;
}

static bool read_idcode(int fd,
                        uint32_t speed_hz,
                        enum sync_mode mode,
                        uint32_t *idcode)
{
    struct spi_ioc_transfer transfers[2] = {0};
    uint8_t activation_prefix[24] = {0};
    size_t prefix_length = make_sync_prefix(mode, activation_prefix);
    uint8_t tx[sizeof(activation_prefix) + 1U] = {0};
    memcpy(tx, activation_prefix, prefix_length);
    tx[prefix_length] = reverse_bits(swd_encode_request(
        (struct swd_request){.ap = false, .read = true, .address = 0U}));
    uint8_t rx[5] = {0};
    transfers[0].tx_buf = (uintptr_t)tx;
    transfers[0].len = (uint32_t)(prefix_length + 1U);
    transfers[0].speed_hz = speed_hz;
    transfers[0].bits_per_word = 8U;
    transfers[1].rx_buf = (uintptr_t)rx;
    transfers[1].len = sizeof(rx);
    transfers[1].speed_hz = speed_hz;
    transfers[1].bits_per_word = 8U;
    if (ioctl(fd, SPI_IOC_MESSAGE(2), transfers) < 0) {
        return false;
    }
    return decode_response(rx, idcode);
}

static bool read_request(int fd,
                         uint32_t speed_hz,
                         enum sync_mode mode,
                         struct swd_request request,
                         uint32_t *value)
{
    struct spi_ioc_transfer transfers[2] = {0};
    uint8_t prefix[24] = {0};
    size_t prefix_length = make_sync_prefix(mode, prefix);
    uint8_t tx[sizeof(prefix) + 1U] = {0};
    memcpy(tx, prefix, prefix_length);
    tx[prefix_length] = reverse_bits(swd_encode_request(request));
    uint8_t rx[5] = {0};
    transfers[0].tx_buf = (uintptr_t)tx;
    transfers[0].len = (uint32_t)(prefix_length + 1U);
    transfers[0].speed_hz = speed_hz;
    transfers[0].bits_per_word = 8U;
    transfers[1].rx_buf = (uintptr_t)rx;
    transfers[1].len = sizeof(rx);
    transfers[1].speed_hz = speed_hz;
    transfers[1].bits_per_word = 8U;
    return ioctl(fd, SPI_IOC_MESSAGE(2), transfers) >= 0 &&
           decode_response(rx, value);
}

static bool read_combined_probe(int fd,
                                uint32_t speed_hz,
                                enum sync_mode mode,
                                uint32_t *idcode,
                                uint32_t *ctrl_stat,
                                uint32_t *ap_idr,
                                bool *id_ok,
                                bool *dp_ok,
                                bool *ap_ok)
{
    struct spi_ioc_transfer transfers[6] = {0};
    uint8_t prefix[24] = {0};
    uint8_t tx[3][sizeof(prefix) + 1U] = {{0}};
    uint8_t rx[3][5] = {{0}};
    struct swd_request requests[3] = {
        {.ap = false, .read = true, .address = 0U},
        {.ap = false, .read = true, .address = 4U},
        {.ap = true, .read = true, .address = 0U},
    };
    size_t prefix_length = make_sync_prefix(mode, prefix);
    for (size_t index = 0U; index < 3U; ++index) {
        memcpy(tx[index], prefix, prefix_length);
        tx[index][prefix_length] = reverse_bits(
            swd_encode_request(requests[index]));
        transfers[2U * index].tx_buf = (uintptr_t)tx[index];
        transfers[2U * index].len = (uint32_t)(prefix_length + 1U);
        transfers[2U * index].speed_hz = speed_hz;
        transfers[2U * index].bits_per_word = 8U;
        transfers[2U * index + 1U].rx_buf = (uintptr_t)rx[index];
        transfers[2U * index + 1U].len = sizeof(rx[index]);
        transfers[2U * index + 1U].speed_hz = speed_hz;
        transfers[2U * index + 1U].bits_per_word = 8U;
    }
    if (ioctl(fd, SPI_IOC_MESSAGE(6), transfers) < 0) {
        *id_ok = false;
        *dp_ok = false;
        *ap_ok = false;
        return false;
    }
    *id_ok = decode_response(rx[0], idcode);
    *dp_ok = decode_response(rx[1], ctrl_stat);
    *ap_ok = decode_response(rx[2], ap_idr);
    return *id_ok && *dp_ok && *ap_ok;
}

static bool read_chunk(int fd,
                       uint32_t speed_hz,
                       size_t first_transaction,
                       size_t count,
                       size_t total_transactions,
                       uint32_t *words)
{
    struct spi_ioc_transfer transfers[2U * spi_chunk_words] = {0};
    uint8_t tx[spi_chunk_words][2] = {{0}};
    uint8_t rx[spi_chunk_words][5] = {{0}};

    for (size_t index = 0U; index < count; ++index) {
        size_t transaction = first_transaction + index;
        bool last = transaction + 1U == total_transactions;
        /* Transaction zero is the discarded AP read that primes the posted
         * MEM-AP pipeline. The final transaction is a DP read that drains the
         * response for the final AP read. */
        struct swd_request request = {
            .ap = !last,
            .read = true,
            .address = 12U,
        };
        tx[index][0] = 0xffU;
        tx[index][1] = reverse_bits(swd_encode_request(request));
        transfers[2U * index].tx_buf = (uintptr_t)tx[index];
        transfers[2U * index].len = sizeof(tx[index]);
        transfers[2U * index].speed_hz = speed_hz;
        transfers[2U * index].bits_per_word = 8U;
        transfers[2U * index + 1U].rx_buf = (uintptr_t)rx[index];
        transfers[2U * index + 1U].len = sizeof(rx[index]);
        transfers[2U * index + 1U].speed_hz = speed_hz;
        transfers[2U * index + 1U].bits_per_word = 8U;
    }

    if (ioctl(fd,
              SPI_IOC_MESSAGE(2U * spi_chunk_words),
              transfers) < 0) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        size_t transaction = first_transaction + index;
        if (transaction != 0U &&
            !decode_response(rx[index], &words[transaction - 1U])) {
            return false;
        }
    }
    return true;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--address HEX] [--words COUNT] "
            "[--speed HZ] [--sync MODE] [--probe-only] [--combined-probe] "
            "[--output FILE]\n"
            "  MODE: none idle1 idle7 line-reset swj swj-reset\n",
            program);
}

int main(int argc, char **argv)
{
    uint32_t address = UINT32_C(0x24000000);
    size_t words_count = 256U;
    uint32_t speed_hz = 12000000U;
    enum sync_mode mode = sync_swj;
    bool probe_only = false;
    bool combined_probe = false;
    const char *output_path = NULL;
    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--address") == 0 &&
            index + 1 < argc && parse_unsigned(argv[++index], &value)) {
            address = (uint32_t)value;
        } else if (strcmp(argv[index], "--words") == 0 &&
                   index + 1 < argc && parse_unsigned(argv[++index], &value) &&
                   value > 0UL) {
            words_count = (size_t)value;
        } else if (strcmp(argv[index], "--speed") == 0 &&
                   index + 1 < argc && parse_unsigned(argv[++index], &value) &&
                   value > 0UL && value <= UINT32_MAX) {
            speed_hz = (uint32_t)value;
        } else if (strcmp(argv[index], "--sync") == 0 &&
                   index + 1 < argc &&
                   parse_sync_mode(argv[++index], &mode)) {
            /* parsed */
        } else if (strcmp(argv[index], "--probe-only") == 0) {
            probe_only = true;
        } else if (strcmp(argv[index], "--combined-probe") == 0) {
            combined_probe = true;
        } else if (strcmp(argv[index], "--output") == 0 &&
                   index + 1 < argc) {
            output_path = argv[++index];
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    uint32_t *words = calloc(words_count, sizeof(uint32_t));
    if (words == NULL) {
        perror("calloc");
        return EXIT_FAILURE;
    }
    struct gpio_mmio gpio = {0};
    if (!prepare_target(address, &gpio)) {
        fprintf(stderr, "CPU SWD setup failed\n");
        free(words);
        return EXIT_FAILURE;
    }

    int fd = open("/dev/spidev0.0", O_RDWR | O_CLOEXEC);
    if (fd < 0 || !configure_spi(fd, speed_hz)) {
        perror("SPI configuration");
        if (fd >= 0) {
            (void)close(fd);
        }
        gpio_mmio_close(&gpio);
        free(words);
        return EXIT_FAILURE;
    }

    /* Put GPIO11/GPIO10 under SPI0 control. GPIO10 remains the shared MOMI
     * line; SPI_3WIRE controls its direction for each transfer. */
    uint32_t original_fsel = gpio.regs[gpio_fsel1];
    uint32_t spi_fsel = original_fsel & ~((7U << 0U) | (7U << 3U));
    spi_fsel |= (4U << 0U) | (4U << 3U);
    /* The CPU backend leaves SWCLK high after its final cycle. SPI0 mode 0
     * starts with a low clock idle level; remove that stale half-cycle before
     * handing the pins to the peripheral. Keep SWDIO high as SWD idle. */
    gpio.regs[gpio_clr0] = 1U << 11U;
    gpio.regs[gpio_set0] = 1U << 10U;
    gpio.regs[gpio_fsel1] = spi_fsel;
    __sync_synchronize();

    uint32_t spi_idcode = 0U;
    uint32_t ctrl_stat = 0U;
    uint32_t ap_idr = 0U;
    bool id_ok = false;
    bool dp_ok = false;
    bool ap_ok = false;
    bool success = false;
    if (combined_probe) {
        (void)read_combined_probe(fd,
                                  speed_hz,
                                  mode,
                                  &spi_idcode,
                                  &ctrl_stat,
                                  &ap_idr,
                                  &id_ok,
                                  &dp_ok,
                                  &ap_ok);
        success = id_ok;
    } else {
        id_ok = read_idcode(fd, speed_hz, mode, &spi_idcode);
        success = id_ok;
    }
    if (success) {
        fprintf(stderr, "SPI 3-wire handoff IDCODE: 0x%08" PRIx32 "\n",
                spi_idcode);
        if (!combined_probe) {
            dp_ok = read_request(fd,
                                 speed_hz,
                                 mode,
                                 (struct swd_request){
                                     .ap = false, .read = true, .address = 4U},
                                 &ctrl_stat);
            ap_ok = read_request(fd,
                                 speed_hz,
                                 mode,
                                 (struct swd_request){
                                     .ap = true, .read = true, .address = 0U},
                                 &ap_idr);
        }
        fprintf(stderr,
                "SPI 3-wire post-handoff DP CTRL/STAT %s=0x%08" PRIx32
                ", AP IDR %s=0x%08" PRIx32 "\n",
                dp_ok ? "ok" : "fail", ctrl_stat,
                ap_ok ? "ok" : "fail", ap_idr);
    }
    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    if (!probe_only && success) {
        size_t total_transactions = words_count + 1U;
        for (size_t first = 0U; first < total_transactions && success;
             first += spi_chunk_words) {
            size_t count = total_transactions - first;
            if (count > spi_chunk_words) {
                count = spi_chunk_words;
            }
            success = read_chunk(fd,
                                 speed_hz,
                                 first,
                                 count,
                                 total_transactions,
                                 words);
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);

    gpio.regs[gpio_fsel1] = original_fsel;
    __sync_synchronize();
    (void)close(fd);
    gpio_mmio_close(&gpio);

    if (!success) {
        printf("SPI_RESULT sync=%s speed=%u id=%s dp=%s ap=%s burst=%s\n",
               sync_mode_name(mode), speed_hz,
               id_ok ? "ok" : "fail",
               dp_ok ? "ok" : "fail",
               ap_ok ? "ok" : "fail",
               probe_only ? "not-run" : "fail");
        free(words);
        return EXIT_FAILURE;
    }
    if (output_path != NULL) {
        FILE *output = fopen(output_path, "wb");
        if (output == NULL ||
            fwrite(words, sizeof(uint32_t), words_count, output) != words_count) {
            fprintf(stderr, "cannot write output file '%s'\n", output_path);
            if (output != NULL) {
                (void)fclose(output);
            }
            free(words);
            return EXIT_FAILURE;
        }
        (void)fclose(output);
    }
    double seconds = elapsed_seconds(&start, &end);
    printf("SPI 3-wire burst read: %zu bytes in %.6f s (%.1f KiB/s, "
           "speed=%u Hz)\n",
           words_count * sizeof(uint32_t),
           seconds,
           ((double)words_count * sizeof(uint32_t) / 1024.0) / seconds,
           speed_hz);
    printf("SPI_RESULT sync=%s speed=%u id=%s dp=%s ap=%s burst=%s\n",
           sync_mode_name(mode), speed_hz,
           id_ok ? "ok" : "fail",
           dp_ok ? "ok" : "fail",
           ap_ok ? "ok" : "fail",
           probe_only ? "not-run" : "ok");
    free(words);
    return EXIT_SUCCESS;
}
