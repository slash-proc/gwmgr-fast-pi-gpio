#define _GNU_SOURCE

/* Minimal kernel-SPI 3-wire SWD probe. This tests the documented half-duplex
 * SPI path: one TX-only transfer followed by one RX-only transfer. */

#include <fcntl.h>
#include <inttypes.h>
#include <linux/spi/spidev.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool response_bit(const uint8_t response[5], unsigned bit)
{
    return (response[bit / 8U] &
            (uint8_t)(1U << (7U - (bit % 8U)))) != 0U;
}

static bool decode_response(const uint8_t response[5], uint32_t *idcode)
{
    unsigned ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (response_bit(response, 1U + bit)) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr,
                "SPI 3-wire ACK=0x%x raw=%02x%02x%02x%02x%02x\n",
                ack,
                response[0], response[1], response[2],
                response[3], response[4]);
        return false;
    }

    uint32_t value = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (response_bit(response, 4U + bit)) {
            value |= UINT32_C(1) << bit;
            ones++;
        }
    }
    bool parity = response_bit(response, 36U);
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr, "SPI 3-wire parity failure\n");
        return false;
    }
    *idcode = value;
    return true;
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : "/dev/spidev0.0";
    uint32_t speed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 1000000U;
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
        return 1;
    }

    uint32_t mode = SPI_NO_CS | SPI_3WIRE;
    uint8_t bits = 8U;
    if (ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("SPI configuration");
        (void)close(fd);
        return 1;
    }

    /* SWD bit order is LSB-first. These bytes are bit-reversed for SPI's
     * MSB-first shifter. The DP IDCODE request 0xa5 is self-reversing. */
    const uint8_t output[] = {
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0x79U, 0xe7U,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0x00U,
        0xa5U,
    };
    uint8_t response[5] = {0};
    struct spi_ioc_transfer transfers[2] = {
        {
            .tx_buf = (uintptr_t)output,
            .len = sizeof(output),
            .speed_hz = speed,
            .bits_per_word = bits,
        },
        {
            .rx_buf = (uintptr_t)response,
            .len = sizeof(response),
            .speed_hz = speed,
            .bits_per_word = bits,
        },
    };
    if (ioctl(fd, SPI_IOC_MESSAGE(2), transfers) < 0) {
        perror("SPI 3-wire transfer");
        (void)close(fd);
        return 1;
    }
    (void)close(fd);

    uint32_t idcode = 0U;
    if (!decode_response(response, &idcode)) {
        return 1;
    }
    printf("SPI 3-wire IDCODE: 0x%08" PRIx32 "\n", idcode);
    return 0;
}
