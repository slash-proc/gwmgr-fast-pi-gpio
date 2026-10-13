#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    static const uint8_t tx[] = {0xa5U, 0x3cU, 0x96U, 0x5aU};
    uint8_t rx[sizeof(tx)] = {0};
    uint32_t mode = SPI_NO_CS;
    uint8_t bits = 8U;
    uint32_t speed = 1000000U;
    struct spi_ioc_transfer transfer = {
        .tx_buf = (uintptr_t)tx,
        .rx_buf = (uintptr_t)rx,
        .len = sizeof(tx),
        .speed_hz = speed,
        .bits_per_word = bits,
    };

    int fd = open("/dev/spidev0.0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open spidev: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(fd, SPI_IOC_WR_MODE32, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
        ioctl(fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
        fprintf(stderr, "SPI loopback ioctl: %s\n", strerror(errno));
        (void)close(fd);
        return 1;
    }
    (void)close(fd);

    printf("tx=");
    for (size_t index = 0U; index < sizeof(tx); ++index) {
        printf("%02x", tx[index]);
    }
    printf(" rx=");
    for (size_t index = 0U; index < sizeof(rx); ++index) {
        printf("%02x", rx[index]);
    }
    printf(" %s\n", memcmp(tx, rx, sizeof(tx)) == 0 ? "PASS" : "FAIL");
    return memcmp(tx, rx, sizeof(tx)) == 0 ? 0 : 1;
}
