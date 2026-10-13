#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    page_size = 4096,
    gpio_set0 = 0x1c / sizeof(uint32_t),
    gpio_clr0 = 0x28 / sizeof(uint32_t),
    gpio_lev0 = 0x34 / sizeof(uint32_t),
    gpio_fsel1 = 0x04 / sizeof(uint32_t),
    spi_cs = 0x00 / sizeof(uint32_t),
    spi_fifo = 0x04 / sizeof(uint32_t),
    spi_clk = 0x08 / sizeof(uint32_t),
    cs_cpha = 1U << 2,
    cs_cpol = 1U << 3,
    cs_clear_tx = 1U << 4,
    cs_clear_rx = 1U << 5,
    cs_ta = 1U << 7,
    cs_ren = 1U << 12,
    cs_done = 1U << 16,
    cs_rxd = 1U << 17,
    cs_txd = 1U << 18,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define GPIO_BASE (PERIPHERAL_BASE + UINT32_C(0x00200000))
#define SPI0_BASE (PERIPHERAL_BASE + UINT32_C(0x00204000))

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

static bool wait_for(volatile uint32_t *spi, uint32_t mask)
{
    for (unsigned attempt = 0U; attempt < 10000000U; ++attempt) {
        if ((spi[spi_cs] & mask) != 0U) {
            return true;
        }
    }
    return false;
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

static bool spi_send_byte(volatile uint32_t *spi, uint8_t value, uint8_t *received)
{
    if (!wait_for(spi, cs_txd)) {
        return false;
    }
    spi[spi_fifo] = value;
    if (!wait_for(spi, cs_rxd)) {
        return false;
    }
    uint8_t result = (uint8_t)spi[spi_fifo];
    if (received != NULL) {
        *received = result;
    }
    return true;
}

static bool spi_set_receive(volatile uint32_t *spi, bool receive)
{
    if (!receive) {
        uint32_t control = spi[spi_cs] & ~(uint32_t)cs_ren;
        spi[spi_cs] = control;
        return true;
    }
    /* REN changes the data direction. Stop TA first, as the kernel
     * spi-bcm2835 driver does when it starts a 3-wire RX transfer. */
    spi[spi_cs] = 0U;
    uint32_t control = cs_clear_tx | cs_clear_rx | cs_ta | cs_ren;
    spi[spi_cs] = control;
    return true;
}

static uint8_t swd_request(bool ap, bool read, uint8_t address)
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

static bool response_bit(const uint8_t response[5], unsigned bit)
{
    return (response[bit / 8U] & (1U << (7U - (bit % 8U)))) != 0U;
}

static bool gpio_read_response(volatile uint32_t *gpio,
                               uint8_t response[5],
                               uint32_t *value)
{
    const uint32_t swclk_mask = 1U << 11U;
    const uint32_t swdio_mask = 1U << 10U;
    memset(response, 0, 5U);

    for (unsigned bit = 0U; bit < 36U; ++bit) {
        gpio[gpio_clr0] = swclk_mask;
        __sync_synchronize();
        bool high = (gpio[gpio_lev0] & swdio_mask) != 0U;
        gpio[gpio_set0] = swclk_mask;
        __sync_synchronize();
        if (high) {
            response[bit / 8U] |= (uint8_t)(1U << (7U - (bit % 8U)));
        }
    }
    fprintf(stderr, "GPIO raw response=%02x%02x%02x%02x%02x\n",
            response[0], response[1], response[2], response[3], response[4]);

    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (response_bit(response, bit)) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr,
                "GPIO response ACK=0x%x response=%02x%02x%02x%02x%02x\n",
                ack,
                response[0],
                response[1],
                response[2],
                response[3],
                response[4]);
        return false;
    }
    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (response_bit(response, 3U + bit)) {
            result |= 1U << bit;
            ones++;
        }
    }
    bool parity = response_bit(response, 35U);
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr, "GPIO response parity failure\n");
        return false;
    }
    *value = result;
    return true;
}

static bool spi_read_response(volatile uint32_t *spi,
                              uint8_t response[5],
                              uint32_t *value)
{
    /* The first bit is the SWD turnaround; ACK starts at bit 1. */
    memset(response, 0, 5U);
    if (!spi_set_receive(spi, true)) {
        return false;
    }
    for (unsigned index = 0U; index < 5U; ++index) {
        if (!spi_send_byte(spi, 0U, &response[index])) {
            return false;
        }
    }
    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (response_bit(response, 1U + bit)) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr,
                "SPI REN response ACK=0x%x response=%02x%02x%02x%02x%02x\n",
                ack,
                response[0], response[1], response[2], response[3], response[4]);
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
        fprintf(stderr, "SPI REN response parity failure\n");
        return false;
    }
    *value = result;
    return true;
}

static bool send_output(volatile uint32_t *spi, const uint8_t *bytes, size_t count)
{
    if (!spi_set_receive(spi, false)) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (!spi_send_byte(spi, bytes[index], NULL)) {
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    unsigned divider = 100U;
    bool use_ren = false;
    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--ren") == 0) {
            use_ren = true;
        } else if (strcmp(argv[index], "--divider") != 0 ||
            index + 1 >= argc ||
            !parse_unsigned(argv[++index], &value) ||
            value == 0UL || value > UINT16_MAX || (value & 1UL) != 0UL) {
            fprintf(stderr, "Usage: %s [--ren] [--divider EVEN_NONZERO]\n", argv[0]);
            return EXIT_FAILURE;
        }
        divider = (unsigned)value;
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
        return EXIT_FAILURE;
    }

    uint32_t original_fsel = gpio[gpio_fsel1];
    uint32_t fsel_spi0 = original_fsel;
    fsel_spi0 &= ~((7U << 0U) | (7U << 3U));
    fsel_spi0 |= (4U << 0U) | (4U << 3U);
    gpio[gpio_fsel1] = fsel_spi0;

    /* CPOL=0, CPHA=0: SWD changes data while clock is low and samples high. */
    spi[spi_cs] = cs_clear_tx | cs_clear_rx;
    spi[spi_clk] = divider;
    spi[spi_cs] = cs_clear_tx | cs_clear_rx | cs_ta;

    const uint8_t line_reset[7] = {0xffU, 0xffU, 0xffU, 0xffU,
                                   0xffU, 0xffU, 0xffU};
    const uint8_t swj_sequence[2] = {0x79U, 0xe7U};
    const uint8_t idle_low = 0x00U;
    /* SPI shifts MSB-first, while SWD shifts each request LSB-first. */
    const uint8_t request = reverse_bits(swd_request(false, true, 0U));
    bool valid = send_output(spi, line_reset, sizeof(line_reset)) &&
                 send_output(spi, swj_sequence, sizeof(swj_sequence)) &&
                 send_output(spi, line_reset, sizeof(line_reset)) &&
                 send_output(spi, &idle_low, 1U) &&
                 send_output(spi, &request, 1U);

    uint8_t response[5] = {0};
    uint32_t idcode = 0U;
    if (valid) {
        if (use_ren) {
            valid = spi_read_response(spi, response, &idcode);
            if (valid) {
                valid = spi_set_receive(spi, false) &&
                        spi_send_byte(spi, request, NULL) &&
                        spi_read_response(spi, response, &idcode);
            }
        } else {
            spi[spi_cs] = 0U;
            uint32_t fsel_gpio = original_fsel & ~((7U << 0U) | (7U << 3U));
            fsel_gpio |= 1U << 3U;
            gpio[gpio_fsel1] = fsel_gpio;
            valid = gpio_read_response(gpio, response, &idcode);
        }
    }
    spi[spi_cs] = 0U;
    gpio[gpio_fsel1] = original_fsel;
    unmap_registers(spi, spi_fd);
    unmap_registers(gpio, gpio_fd);

    if (!valid) {
        return EXIT_FAILURE;
    }
    printf("SPI SWD IDCODE: 0x%08" PRIx32 " (CLK divider %u%s)\n",
           idcode,
           divider,
           use_ren ? ", REN" : "");
    return EXIT_SUCCESS;
}
