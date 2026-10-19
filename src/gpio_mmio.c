#define _GNU_SOURCE

#include "gpio_mmio.h"
#include "swd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef GWMGR_FAST_PI_GPIO_GPIO_PHYS_BASE
#define GWMGR_FAST_PI_GPIO_GPIO_PHYS_BASE UINT32_C(0xfe200000)
#endif

enum {
    GPIO_FSEL0 = 0x00 / 4,
    GPIO_GPSET0 = 0x1c / 4,
    GPIO_GPCLR0 = 0x28 / 4,
    GPIO_GPLEV0 = 0x34 / 4
};

static inline void gpio_mmio_store_barrier(void)
{
#if defined(GWMGR_FAST_PI_GPIO_NO_BARRIERS) || defined(GWMGR_FAST_PI_GPIO_NO_STORE_BARRIERS)
    __asm__ __volatile__("" ::: "memory");
#elif defined(__aarch64__) && defined(GWMGR_FAST_PI_GPIO_STORE_BARRIERS) && !defined(GWMGR_FAST_PI_GPIO_FULL_BARRIERS)
    __asm__ __volatile__("dmb ishst" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static inline void gpio_mmio_io_barrier(void)
{
#if defined(GWMGR_FAST_PI_GPIO_NO_BARRIERS) || \
    defined(GWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS)
    __asm__ __volatile__("" ::: "memory");
#elif defined(__aarch64__) && defined(GWMGR_FAST_PI_GPIO_READ_BARRIERS) && !defined(GWMGR_FAST_PI_GPIO_FULL_BARRIERS)
    __asm__ __volatile__("dmb ishld" ::: "memory");
#elif defined(__aarch64__) && defined(GWMGR_FAST_PI_GPIO_STORE_BARRIERS) && !defined(GWMGR_FAST_PI_GPIO_FULL_BARRIERS)
    __asm__ __volatile__("dmb ish" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static void gpio_set_function(struct gpio_mmio *gpio, unsigned pin, unsigned function)
{
    unsigned register_index = pin / 10U;
    unsigned shift = (pin % 10U) * 3U;
    unsigned int value = gpio->regs[GPIO_FSEL0 + register_index];
    value &= ~(7U << shift);
    value |= (function & 7U) << shift;
    gpio->regs[GPIO_FSEL0 + register_index] = value;
    gpio_mmio_store_barrier();
}

static void gpio_write_pin(struct gpio_mmio *gpio, unsigned pin, bool high)
{
    unsigned register_index = pin / 32U;
    unsigned bit = 1U << (pin % 32U);
    gpio->regs[(high ? GPIO_GPSET0 : GPIO_GPCLR0) + register_index] = bit;
    gpio_mmio_store_barrier();
}

int gpio_mmio_open(struct gpio_mmio *gpio,
                   unsigned swclk_pin,
                   unsigned swdio_pin,
                   unsigned clock_hz)
{
    if (gpio == NULL || clock_hz == 0U || swclk_pin > 53U || swdio_pin > 53U) {
        return EINVAL;
    }

    int fd;
#if defined(GWMGR_FAST_PI_GPIO_USE_DEVMEM)
    fd = open("/dev/mem", O_RDWR | O_SYNC);
#else
    fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
#endif
    if (fd < 0) {
        return errno;
    }

#if defined(GWMGR_FAST_PI_GPIO_USE_DEVMEM)
    off_t mapping_offset = (off_t)GWMGR_FAST_PI_GPIO_GPIO_PHYS_BASE;
#else
    off_t mapping_offset = 0;
#endif
    void *mapping = mmap(NULL,
                         4096,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         fd,
                         mapping_offset);
    int saved_errno = errno;
    (void)close(fd);
    if (mapping == MAP_FAILED) {
        return saved_errno;
    }

    gpio->regs = (volatile unsigned int *)mapping;
    gpio->swclk_pin = swclk_pin;
    gpio->swdio_pin = swdio_pin;
    unsigned swclk_register = swclk_pin / 32U;
    unsigned swdio_register = swdio_pin / 32U;
    unsigned swdio_fsel_register = swdio_pin / 10U;
    unsigned swdio_fsel_shift = (swdio_pin % 10U) * 3U;
    gpio->swclk_set = &gpio->regs[GPIO_GPSET0 + swclk_register];
    gpio->swclk_clear = &gpio->regs[GPIO_GPCLR0 + swclk_register];
    gpio->swdio_set = &gpio->regs[GPIO_GPSET0 + swdio_register];
    gpio->swdio_clear = &gpio->regs[GPIO_GPCLR0 + swdio_register];
    gpio->swdio_level = &gpio->regs[GPIO_GPLEV0 + swdio_register];
    gpio->swdio_fsel = &gpio->regs[GPIO_FSEL0 + swdio_fsel_register];
    gpio->swclk_mask = 1U << (swclk_pin % 32U);
    gpio->swdio_mask = 1U << (swdio_pin % 32U);
    gpio->half_cycle_ns = 1000000000UL / ((unsigned long)clock_hz * 2UL);
    if (gpio->half_cycle_ns == 0UL) {
        gpio->half_cycle_ns = 1UL;
    }

    gpio_set_function(gpio, swclk_pin, 1U);
    gpio_set_function(gpio, swdio_pin, 0U);
    uint32_t swdio_fsel_mask = 7U << swdio_fsel_shift;
    uint32_t swdio_fsel_value = *gpio->swdio_fsel;
    gpio->swdio_fsel_input = swdio_fsel_value & ~swdio_fsel_mask;
    gpio->swdio_fsel_output = gpio->swdio_fsel_input | (1U << swdio_fsel_shift);
    gpio->swdio_is_output = false;
    gpio->swdio_level_valid = false;
    gpio->swdio_output_level = false;
    gpio_write_pin(gpio, swclk_pin, true);
    return 0;
}

void gpio_mmio_close(struct gpio_mmio *gpio)
{
    if (gpio != NULL && gpio->regs != NULL) {
        gpio_set_function(gpio, gpio->swclk_pin, 0U);
        gpio_set_function(gpio, gpio->swdio_pin, 0U);
        (void)munmap((void *)gpio->regs, 4096);
        gpio->regs = NULL;
    }
}

void gpio_mmio_set_swdio_output(void *ctx, bool output)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    if (gpio->swdio_is_output == output) {
        return;
    }
    *gpio->swdio_fsel = output ? gpio->swdio_fsel_output
                               : gpio->swdio_fsel_input;
#if !defined(GWMGR_FAST_PI_GPIO_NO_FSEL_BARRIERS)
    gpio_mmio_store_barrier();
#endif
    gpio->swdio_is_output = output;
    gpio->swdio_level_valid = false;
}

void gpio_mmio_write_swdio(void *ctx, bool high)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    if (!gpio->swdio_level_valid || gpio->swdio_output_level != high) {
        *(high ? gpio->swdio_set : gpio->swdio_clear) = gpio->swdio_mask;
        gpio_mmio_store_barrier();
    }
    gpio->swdio_output_level = high;
    gpio->swdio_level_valid = true;
}

bool gpio_mmio_read_swdio(void *ctx)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    return (*gpio->swdio_level & gpio->swdio_mask) != 0U;
}

void gpio_mmio_write_swclk(void *ctx, bool high)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    *(high ? gpio->swclk_set : gpio->swclk_clear) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
}

void gpio_mmio_wait_half_cycle(void *ctx)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    /* At high requested rates, the MMIO/callback path is already slower than
     * the requested half-cycle. Avoid paying for a clock_gettime() call that
     * cannot improve the actual waveform. */
    if (gpio->half_cycle_ns < 100UL) {
        return;
    }
    struct timespec start;
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);

    unsigned long elapsed;
    do {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC_RAW, &now);
        elapsed = (unsigned long)(now.tv_sec - start.tv_sec) * 1000000000UL;
        if (now.tv_nsec >= start.tv_nsec) {
            elapsed += (unsigned long)(now.tv_nsec - start.tv_nsec);
        } else {
            elapsed -= (unsigned long)(start.tv_nsec - now.tv_nsec);
        }
    } while (elapsed < gpio->half_cycle_ns);
}

void gpio_mmio_clock_cycle(void *ctx)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    *(gpio->swclk_clear) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
    gpio_mmio_wait_half_cycle(gpio);
    *(gpio->swclk_set) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
    gpio_mmio_wait_half_cycle(gpio);
}

void gpio_mmio_write_bit_cycle(void *ctx, bool bit)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    if (!gpio->swdio_level_valid || gpio->swdio_output_level != bit) {
        *(bit ? gpio->swdio_set : gpio->swdio_clear) = gpio->swdio_mask;
        gpio->swdio_output_level = bit;
        gpio->swdio_level_valid = true;
    }
    *(gpio->swclk_clear) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
    gpio_mmio_wait_half_cycle(gpio);
    *(gpio->swclk_set) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
    gpio_mmio_wait_half_cycle(gpio);
}

bool gpio_mmio_read_bit_cycle(void *ctx)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    *(gpio->swclk_clear) = gpio->swclk_mask;
    gpio_mmio_io_barrier();
    gpio_mmio_wait_half_cycle(gpio);
    bool bit = (*gpio->swdio_level & gpio->swdio_mask) != 0U;
    *(gpio->swclk_set) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
    gpio_mmio_wait_half_cycle(gpio);
    return bit;
}

void gpio_mmio_write_bits(void *ctx, uint32_t value, unsigned count)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    for (unsigned bit = 0U; bit < count; ++bit) {
        bool high = ((value >> bit) & 1U) != 0U;
        if (!gpio->swdio_level_valid || gpio->swdio_output_level != high) {
            if (high) {
                *(gpio->swdio_set) = gpio->swdio_mask;
            } else {
                *(gpio->swdio_clear) = gpio->swdio_mask;
            }
            gpio->swdio_output_level = high;
            gpio->swdio_level_valid = true;
        }
        *(gpio->swclk_clear) = gpio->swclk_mask;
        gpio_mmio_store_barrier();
        gpio_mmio_wait_half_cycle(gpio);
        *(gpio->swclk_set) = gpio->swclk_mask;
        gpio_mmio_store_barrier();
        gpio_mmio_wait_half_cycle(gpio);
    }
}

uint32_t gpio_mmio_read_bits(void *ctx, unsigned count)
{
    struct gpio_mmio *gpio = (struct gpio_mmio *)ctx;
    uint32_t value = 0U;
    for (unsigned bit = 0U; bit < count; ++bit) {
        *(gpio->swclk_clear) = gpio->swclk_mask;
        gpio_mmio_io_barrier();
        gpio_mmio_wait_half_cycle(gpio);
        if ((*gpio->swdio_level & gpio->swdio_mask) != 0U) {
            value |= 1U << bit;
        }
        *(gpio->swclk_set) = gpio->swclk_mask;
        gpio_mmio_store_barrier();
        gpio_mmio_wait_half_cycle(gpio);
    }
    return value;
}

static inline void gpio_fast_clock(struct gpio_mmio *gpio)
{
    *(gpio->swclk_clear) = gpio->swclk_mask;
    gpio_mmio_store_barrier();
    *(gpio->swclk_set) = gpio->swclk_mask;
#if !defined(GWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS)
    gpio_mmio_store_barrier();
#endif
}

static inline void gpio_fast_set_output(struct gpio_mmio *gpio, bool output)
{
    if (gpio->swdio_is_output == output) {
        return;
    }
    *gpio->swdio_fsel = output ? gpio->swdio_fsel_output
                               : gpio->swdio_fsel_input;
    gpio_mmio_store_barrier();
    gpio->swdio_is_output = output;
    gpio->swdio_level_valid = false;
}

static inline void gpio_fast_write_bit(struct gpio_mmio *gpio, bool high)
{
    bool swdio_changes =
        !gpio->swdio_level_valid || gpio->swdio_output_level != high;
    /* Keep SWDIO stable before driving the falling SWCLK edge.  Combining a
     * low SWDIO transition with GPCLR for SWCLK removes the setup interval;
     * it can pass read tests while corrupting target RAM writes. */
    if (swdio_changes) {
        *(high ? gpio->swdio_set : gpio->swdio_clear) = gpio->swdio_mask;
#if !defined(GWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS)
        gpio_mmio_store_barrier();
#endif
        gpio->swdio_output_level = high;
        gpio->swdio_level_valid = true;
    }
    gpio_fast_clock(gpio);
}

static inline bool gpio_fast_read_bit(struct gpio_mmio *gpio)
{
    *(gpio->swclk_clear) = gpio->swclk_mask;
    gpio_mmio_io_barrier();
    bool high = (*gpio->swdio_level & gpio->swdio_mask) != 0U;
    *(gpio->swclk_set) = gpio->swclk_mask;
#if !defined(GWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS)
    gpio_mmio_store_barrier();
#endif
    return high;
}

static inline void gpio_fast_write_bits(struct gpio_mmio *gpio,
                                        uint32_t value,
                                        unsigned count)
{
    for (unsigned bit = 0U; bit < count; ++bit) {
        gpio_fast_write_bit(gpio, ((value >> bit) & 1U) != 0U);
    }
}

static inline uint32_t gpio_fast_read_bits(struct gpio_mmio *gpio,
                                           unsigned count)
{
    uint32_t value = 0U;
    for (unsigned bit = 0U; bit < count; ++bit) {
        if (gpio_fast_read_bit(gpio)) {
            value |= UINT32_C(1) << bit;
        }
    }
    return value;
}

static inline void gpio_fast_turnaround_to_input(struct gpio_mmio *gpio)
{
    gpio_fast_set_output(gpio, false);
    gpio_fast_clock(gpio);
}

static inline void gpio_fast_turnaround_to_output(struct gpio_mmio *gpio)
{
    gpio_fast_clock(gpio);
    gpio_fast_set_output(gpio, true);
    *(gpio->swdio_set) = gpio->swdio_mask;
#if !defined(GWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS)
    gpio_mmio_store_barrier();
#endif
    gpio->swdio_output_level = true;
    gpio->swdio_level_valid = true;
}

static enum swd_ack gpio_fast_transfer(struct gpio_mmio *gpio,
                                       uint8_t request_byte,
                                       uint32_t write_data,
                                       bool read,
                                       uint32_t *read_data)
{
    gpio_fast_set_output(gpio, true);
    gpio_fast_write_bits(gpio, request_byte, 8U);
    gpio_fast_turnaround_to_input(gpio);

    enum swd_ack ack = (enum swd_ack)gpio_fast_read_bits(gpio, 3U);
    if (ack != SWD_ACK_OK) {
        gpio_fast_turnaround_to_output(gpio);
        return ack;
    }
    if (read) {
        uint32_t value = gpio_fast_read_bits(gpio, 32U);
        bool parity = gpio_fast_read_bit(gpio);
        gpio_fast_turnaround_to_output(gpio);
        if (parity != (__builtin_parity(value) != 0)) {
            return SWD_ACK_INVALID;
        }
        if (read_data != NULL) {
            *read_data = value;
        }
    } else {
        gpio_fast_turnaround_to_output(gpio);
        gpio_fast_write_bits(gpio, write_data, 32U);
        gpio_fast_write_bit(gpio, __builtin_parity(write_data) != 0);
    }
    return SWD_ACK_OK;
}

static bool gpio_fast_transfer_retry(struct gpio_mmio *gpio,
                                     uint8_t request_byte,
                                     uint32_t write_data,
                                     bool read,
                                     uint32_t *read_data)
{
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        enum swd_ack ack = gpio_fast_transfer(gpio,
                                               request_byte,
                                               write_data,
                                               read,
                                               read_data);
        if (ack == SWD_ACK_OK) {
            return true;
        }
        /* An accepted AP read with bad parity already advanced the pipeline.
         * Only WAIT can be retried without silently losing a word. */
        if (ack != SWD_ACK_WAIT) {
            return false;
        }
    }
    return false;
}

bool gpio_mmio_fast_read_block(struct gpio_mmio *gpio,
                               void *buffer,
                               size_t length,
                               size_t *bytes_read)
{
    if (gpio == NULL || buffer == NULL || length == 0U ||
        (length % sizeof(uint32_t)) != 0U) {
        return false;
    }
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }

    uint8_t ap_read = swd_encode_request(
        (struct swd_request){.ap = true, .read = true, .address = 12U});
    uint8_t dp_read = swd_encode_request(
        (struct swd_request){.ap = false, .read = true, .address = 12U});
    size_t words = length / sizeof(uint32_t);
    size_t completed = 0U;
    uint32_t discard = 0U;
    if (!gpio_fast_transfer_retry(gpio, ap_read, 0U, true, &discard)) {
        return false;
    }
    uint8_t *destination = (uint8_t *)buffer;
    for (size_t word = 0U; word < words; ++word) {
        uint8_t request = word + 1U == words ? dp_read : ap_read;
        uint32_t value = 0U;
        if (!gpio_fast_transfer_retry(gpio,
                                      request,
                                      0U,
                                      true,
                                      &value)) {
            if (bytes_read != NULL) {
                *bytes_read = completed;
            }
            return false;
        }
        memcpy(destination + word * sizeof(value), &value, sizeof(value));
        completed += sizeof(value);
    }
    if (bytes_read != NULL) {
        *bytes_read = completed;
    }
    return true;
}

bool gpio_mmio_fast_read_memory(struct gpio_mmio *gpio,
                                uint32_t address,
                                void *buffer,
                                size_t length,
                                size_t *bytes_read)
{
    if (gpio == NULL || buffer == NULL || length == 0U ||
        (length % sizeof(uint32_t)) != 0U) {
        return false;
    }
    uint8_t tar_write = swd_encode_request(
        (struct swd_request){.ap = true, .read = false, .address = 4U});
    if (!gpio_fast_transfer_retry(gpio, tar_write, address, false, NULL)) {
        if (bytes_read != NULL) {
            *bytes_read = 0U;
        }
        return false;
    }
    return gpio_mmio_fast_read_block(gpio, buffer, length, bytes_read);
}

bool gpio_mmio_fast_write_memory(struct gpio_mmio *gpio,
                                 uint32_t address,
                                 const void *buffer,
                                 size_t length,
                                 size_t *bytes_written)
{
    if (gpio == NULL || buffer == NULL || length == 0U ||
        (address & 3U) != 0U || (length % sizeof(uint32_t)) != 0U) {
        return false;
    }
    if (bytes_written != NULL) {
        *bytes_written = 0U;
    }

    const uint8_t ap_tar_write = swd_encode_request(
        (struct swd_request){.ap = true, .read = false, .address = 4U});
    const uint8_t ap_write = swd_encode_request(
        (struct swd_request){.ap = true, .read = false, .address = 12U});
    const uint8_t dp_read = swd_encode_request(
        (struct swd_request){.ap = false, .read = true, .address = 12U});
    const uint8_t *source = (const uint8_t *)buffer;
    size_t completed = 0U;

    if (!gpio_fast_transfer_retry(gpio, ap_tar_write, address, false, NULL)) {
        return false;
    }
    const size_t words = length / sizeof(uint32_t);
    for (size_t word = 0U; word < words; ++word) {
        uint32_t value = 0U;
        memcpy(&value, source + word * sizeof(value), sizeof(value));
        if (!gpio_fast_transfer_retry(gpio, ap_write, value, false, NULL)) {
            if (bytes_written != NULL) {
                *bytes_written = completed;
            }
            return false;
        }
        completed += sizeof(value);
    }

    /* Drain the AP write pipeline once per block instead of doing a DP read
     * after every word. The final AP write is complete before this read is
     * acknowledged, and the DP read also provides the normal SWD barrier. */
    uint32_t discard = 0U;
    if (!gpio_fast_transfer_retry(gpio, dp_read, 0U, true, &discard)) {
        if (bytes_written != NULL) {
            *bytes_written = completed;
        }
        return false;
    }
    if (bytes_written != NULL) {
        *bytes_written = completed;
    }
    return true;
}
