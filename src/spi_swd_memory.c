#define _GNU_SOURCE

/*
 * Experimental SWD backend using SPI0 for clocking.
 *
 * GPIO10 remains the target SWDIO connection. SPI_REN puts the SPI0 MOSI
 * driver into high impedance for target responses, so no additional wire is
 * required. This is intentionally a standalone benchmark while the transfer
 * format and direction changes are being validated.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
    gpio_clr0 = 0x28 / sizeof(uint32_t),
    gpio_lev0 = 0x34 / sizeof(uint32_t),
    spi_cs = 0x00 / sizeof(uint32_t),
    spi_fifo = 0x04 / sizeof(uint32_t),
    spi_clk = 0x08 / sizeof(uint32_t),
    cs_cpha = 1U << 2,
    cs_cpol = 1U << 3,
    cs_clear_tx = 1U << 4,
    cs_clear_rx = 1U << 5,
    cs_ta = 1U << 7,
    cs_ren = 1U << 12,
    cs_txd = 1U << 18,
    cs_rxd = 1U << 17,
    cs_done = 1U << 16,
    response_ack_only = 0,
    response_data = 1,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define GPIO_BASE (PERIPHERAL_BASE + UINT32_C(0x00200000))
#define SPI0_BASE (PERIPHERAL_BASE + UINT32_C(0x00204000))

#define GPIO_SWCLK UINT32_C(1) << 11U
#define GPIO_SWDIO UINT32_C(1) << 10U

struct mapped_registers {
    volatile uint32_t *address;
    int fd;
};

struct probe_context {
    volatile uint32_t *gpio;
    volatile uint32_t *spi;
    uint32_t fsel_output;
    uint32_t fsel_input;
    uint32_t fsel_gpio_output;
};

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

static uint8_t reverse_bits(uint8_t value)
{
    value = (uint8_t)(((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U));
    value = (uint8_t)(((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U));
    return (uint8_t)((value << 4U) | (value >> 4U));
}

static void spi_start_output(struct probe_context *context)
{
    context->spi[spi_cs] = 0U;
    context->spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_ta;
}

static void spi_start_receive(struct probe_context *context)
{
    context->spi[spi_cs] = 0U;
    context->spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_ta | cs_ren;
}

static bool gpio_clock_sample(struct probe_context *context);

static void gpio_start_input(struct probe_context *context)
{
    context->spi[spi_cs] = 0U;
    context->gpio[gpio_fsel1] = context->fsel_input;
    __sync_synchronize();
    (void)gpio_clock_sample(context);
}

static bool gpio_clock_sample(struct probe_context *context)
{
    context->gpio[gpio_clr0] = GPIO_SWCLK;
    __sync_synchronize();
    for (unsigned delay = 0U; delay < 128U; ++delay) {
        __asm__ __volatile__("nop");
    }
    bool high = (context->gpio[gpio_lev0] & GPIO_SWDIO) != 0U;
    context->gpio[gpio_set0] = GPIO_SWCLK;
    __sync_synchronize();
    return high;
}

static void gpio_start_output(struct probe_context *context)
{
    /* The target-to-host turnaround is clocked while SWDIO is still input. */
    context->gpio[gpio_fsel1] = context->fsel_input;
    (void)gpio_clock_sample(context);
    context->gpio[gpio_set0] = GPIO_SWDIO;
    context->gpio[gpio_fsel1] = context->fsel_output;
    __sync_synchronize();
    spi_start_output(context);
}

static void gpio_finish_input(struct probe_context *context)
{
    context->gpio[gpio_set0] = GPIO_SWDIO;
    context->gpio[gpio_fsel1] = context->fsel_output;
    __sync_synchronize();
    spi_start_output(context);
}

static bool spi_send_byte(struct probe_context *context, uint8_t value)
{
    for (unsigned attempt = 0U; attempt < 10000000U; ++attempt) {
        if ((context->spi[spi_cs] & cs_txd) != 0U) {
            context->spi[spi_fifo] = value;
            break;
        }
        if (attempt + 1U == 10000000U) {
            return false;
        }
    }
    for (unsigned attempt = 0U; attempt < 10000000U; ++attempt) {
        if ((context->spi[spi_cs] & cs_rxd) != 0U) {
            (void)context->spi[spi_fifo];
            return true;
        }
    }
    return false;
}

static bool spi_receive_byte(struct probe_context *context, uint8_t *value)
{
    for (unsigned attempt = 0U; attempt < 10000000U; ++attempt) {
        if ((context->spi[spi_cs] & cs_txd) != 0U) {
            context->spi[spi_fifo] = 0U;
            break;
        }
        if (attempt + 1U == 10000000U) {
            return false;
        }
    }
    for (unsigned attempt = 0U; attempt < 10000000U; ++attempt) {
        if ((context->spi[spi_cs] & cs_rxd) != 0U) {
            *value = (uint8_t)context->spi[spi_fifo];
            return true;
        }
    }
    return false;
}

static bool send_swd_bytes(struct probe_context *context,
                           const uint8_t *bytes,
                           size_t count)
{
    uint32_t control = context->spi[spi_cs];
    control &= ~((uint32_t)cs_clear_tx | (uint32_t)cs_clear_rx |
                 (uint32_t)cs_done | (uint32_t)cs_ren);
    context->spi[spi_cs] = control;
    for (size_t index = 0U; index < count; ++index) {
        if (!spi_send_byte(context, reverse_bits(bytes[index]))) {
            return false;
        }
    }
    return true;
}

static bool send_spi_bytes(struct probe_context *context,
                           const uint8_t *bytes,
                           size_t count)
{
    uint32_t control = context->spi[spi_cs];
    control &= ~((uint32_t)cs_clear_tx | (uint32_t)cs_clear_rx |
                 (uint32_t)cs_done | (uint32_t)cs_ren);
    context->spi[spi_cs] = control;
    for (size_t index = 0U; index < count; ++index) {
        if (!spi_send_byte(context, bytes[index])) {
            return false;
        }
    }
    return true;
}

static void send_gpio_request(struct probe_context *context, uint8_t request)
{
    context->spi[spi_cs] = 0U;
    context->gpio[gpio_set0] = GPIO_SWDIO;
    context->gpio[gpio_fsel1] = context->fsel_gpio_output;
    __sync_synchronize();
    for (unsigned bit = 0U; bit < 8U; ++bit) {
        if ((request & (uint8_t)(1U << bit)) != 0U) {
            context->gpio[gpio_set0] = GPIO_SWDIO;
        } else {
            context->gpio[gpio_clr0] = GPIO_SWDIO;
        }
        context->gpio[gpio_clr0] = GPIO_SWCLK;
        __sync_synchronize();
        context->gpio[gpio_set0] = GPIO_SWCLK;
        __sync_synchronize();
    }
    context->gpio[gpio_fsel1] = context->fsel_output;
    __sync_synchronize();
    spi_start_output(context);
}

static bool response_bit(const uint8_t *bytes, size_t bit)
{
    return (bytes[bit / 8U] & (uint8_t)(1U << (7U - (bit % 8U)))) != 0U;
}

static bool read_response(struct probe_context *context,
                          uint32_t *value,
                          int response_kind)
{
    uint8_t raw[5] = {0};
    size_t response_bits = response_kind == response_ack_only ? 3U : 36U;
    gpio_start_input(context);
    for (size_t bit = 0U; bit < response_bits; ++bit) {
        if (gpio_clock_sample(context)) {
            raw[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
        }
    }
    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (response_bit(raw, bit)) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr, "SPI SWD ACK=0x%x raw=%02x%02x%02x%02x%02x\n",
                ack, raw[0], raw[1], raw[2], raw[3], raw[4]);
        gpio_start_output(context);
        return false;
    }
    if (response_kind == response_ack_only) {
        gpio_start_output(context);
        return true;
    }
    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (response_bit(raw, 4U + bit)) {
            result |= UINT32_C(1) << bit;
            ones++;
        }
    }
    bool parity = response_bit(raw, 36U);
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr, "SPI SWD parity failure raw=%02x%02x%02x%02x%02x\n",
                raw[0], raw[1], raw[2], raw[3], raw[4]);
        gpio_start_output(context);
        return false;
    }
    *value = result;
    gpio_start_output(context);
    return true;
}

static uint8_t encode_request(bool ap, bool read, uint8_t address)
{
    uint8_t value = 0x81U;
    if (ap) {
        value |= 0x02U;
    }
    if (read) {
        value |= 0x04U;
    }
    value |= (uint8_t)((address & 0x0cU) << 1U);
    unsigned ones = 0U;
    for (unsigned bit = 1U; bit <= 4U; ++bit) {
        ones += (unsigned)((value >> bit) & 1U);
    }
    if ((ones & 1U) != 0U) {
        value |= 0x20U;
    }
    return value;
}

static bool swd_request(struct probe_context *context,
                        bool ap,
                        bool read,
                        uint8_t address,
                        uint32_t write_value,
                        uint32_t *read_value)
{
    uint8_t request = encode_request(ap, read, address);
    send_gpio_request(context, request);
    if (!read_response(context,
                       read_value,
                       read ? response_data : response_ack_only)) {
        fprintf(stderr,
                "SPI SWD transfer failed ap=%u read=%u address=0x%02x\n",
                ap ? 1U : 0U, read ? 1U : 0U, address);
        return false;
    }
    if (!read) {
        uint8_t data[5] = {0};
        for (unsigned bit = 0U; bit < 33U; ++bit) {
            bool high;
            if (bit < 32U) {
                high = ((write_value >> bit) & 1U) != 0U;
            } else {
                unsigned ones = 0U;
                for (unsigned data_bit = 0U; data_bit < 32U; ++data_bit) {
                    ones += (unsigned)((write_value >> data_bit) & 1U);
                }
                high = (ones & 1U) != 0U;
            }
            if (high) {
                data[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
            }
        }
        for (size_t bit = 33U; bit < 40U; ++bit) {
            data[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
        }
        for (size_t index = 0U; index < sizeof(data); ++index) {
            if (!spi_send_byte(context, data[index])) {
                return false;
            }
        }
    }
    return true;
}

static bool switch_to_swd(struct probe_context *context)
{
    uint8_t reset[7];
    uint8_t swj[2] = {0x79U, 0xe7U};
    uint8_t idle[1] = {0x00U};
    memset(reset, 0xff, sizeof(reset));
    return send_spi_bytes(context, reset, sizeof(reset)) &&
           send_spi_bytes(context, swj, sizeof(swj)) &&
           send_spi_bytes(context, reset, sizeof(reset)) &&
           send_spi_bytes(context, idle, sizeof(idle));
}

static bool prepare_memory(struct probe_context *context)
{
    uint32_t status = 0U;
    return swd_request(context, false, false, 0U, 0x0000001eU, NULL) &&
           swd_request(context, false, false, 4U, 0x50000000U, NULL) &&
           swd_request(context, false, true, 4U, 0U, &status) &&
           (status & 0xa0000000U) == 0xa0000000U &&
           swd_request(context, false, false, 8U, 0U, NULL) &&
           swd_request(context, true, false, 0U, 0x23000052U, NULL);
}

static bool read_memory(struct probe_context *context,
                        uint32_t address,
                        uint32_t *words,
                        size_t word_count)
{
    uint32_t discard = 0U;
    if (!swd_request(context, true, false, 4U, address, NULL) ||
        !swd_request(context, true, true, 12U, 0U, &discard)) {
        return false;
    }
    for (size_t index = 0U; index < word_count; ++index) {
        bool last = index + 1U == word_count;
        if (!swd_request(context,
                         last ? false : true,
                         true,
                         12U,
                         0U,
                         &words[index])) {
            return false;
        }
    }
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
    unsigned divider = 20U;
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
        } else {
            fprintf(stderr,
                    "Usage: %s [--address HEX] [--words COUNT] [--divider EVEN]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }
    uint32_t *words_buffer = calloc(words, sizeof(uint32_t));
    if (words_buffer == NULL) {
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
        free(words_buffer);
        return EXIT_FAILURE;
    }

    uint32_t original_fsel = gpio[gpio_fsel1];
    struct probe_context context = {.gpio = gpio, .spi = spi};
    uint32_t fsel_base = original_fsel & ~((7U << 0U) | (7U << 3U));
    context.fsel_output = fsel_base | (4U << 0U) | (4U << 3U);
    /* During response capture SWCLK is a normal GPIO driven by the sampler. */
    context.fsel_input = fsel_base | (1U << 3U);
    context.fsel_gpio_output = fsel_base | (1U << 0U) | (1U << 3U);
    gpio[gpio_fsel1] = (original_fsel & ~((7U << 0U) | (7U << 3U))) |
                       (4U << 0U) | (4U << 3U);
    spi[spi_cs] = cs_clear_tx | cs_clear_rx;
    spi[spi_clk] = divider;
    spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_ta;

    bool success = switch_to_swd(&context);
    uint32_t idcode = 0U;
    success = success && swd_request(&context, false, true, 0U, 0U, &idcode);
    if (success) {
        fprintf(stderr, "SPI SWD initial IDCODE=0x%08" PRIx32 "\n", idcode);
    }
    success = success && prepare_memory(&context);
    struct timespec start = {0};
    struct timespec end = {0};
    if (success) {
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
        success = read_memory(&context, address, words_buffer, words);
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    }
    spi[spi_cs] = 0U;
    gpio[gpio_fsel1] = original_fsel;
    unmap_registers(spi, spi_fd);
    unmap_registers(gpio, gpio_fd);

    if (!success) {
        free(words_buffer);
        return EXIT_FAILURE;
    }
    double seconds = elapsed_seconds(&start, &end);
    printf("SPI SWD IDCODE: 0x%08" PRIx32 "\n", idcode);
    printf("read: %zu bytes in %.3f s (%.1f KiB/s), SPI divider=%u\n",
           words * sizeof(uint32_t), seconds,
           ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
           divider);
    free(words_buffer);
    return EXIT_SUCCESS;
}
