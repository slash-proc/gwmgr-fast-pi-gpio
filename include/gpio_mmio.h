#ifndef RPI_FAST_GPIO_GPIO_MMIO_H
#define RPI_FAST_GPIO_GPIO_MMIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct gpio_mmio {
    volatile unsigned int *regs;
    unsigned swclk_pin;
    unsigned swdio_pin;
    unsigned long half_cycle_ns;
    volatile uint32_t *swclk_set;
    volatile uint32_t *swclk_clear;
    volatile uint32_t *swdio_set;
    volatile uint32_t *swdio_clear;
    volatile uint32_t *swdio_level;
    volatile uint32_t *swdio_fsel;
    uint32_t swclk_mask;
    uint32_t swdio_mask;
    uint32_t swdio_fsel_input;
    uint32_t swdio_fsel_output;
    bool swdio_is_output;
    bool swdio_level_valid;
    bool swdio_output_level;
};

int gpio_mmio_open(struct gpio_mmio *gpio,
                   unsigned swclk_pin,
                   unsigned swdio_pin,
                   unsigned clock_hz);
void gpio_mmio_close(struct gpio_mmio *gpio);

void gpio_mmio_set_swdio_output(void *ctx, bool output);
void gpio_mmio_write_swdio(void *ctx, bool high);
bool gpio_mmio_read_swdio(void *ctx);
void gpio_mmio_write_swclk(void *ctx, bool high);
void gpio_mmio_wait_half_cycle(void *ctx);
void gpio_mmio_clock_cycle(void *ctx);
void gpio_mmio_write_bit_cycle(void *ctx, bool bit);
bool gpio_mmio_read_bit_cycle(void *ctx);
void gpio_mmio_write_bits(void *ctx, uint32_t value, unsigned count);
uint32_t gpio_mmio_read_bits(void *ctx, unsigned count);

/* Fused CPU SWD memory reader. It keeps the GPIO register state and SWD
 * bit-loop in one translation unit, avoiding the per-bit function-pointer
 * dispatch used by the generic protocol interface. */
bool gpio_mmio_fast_read_memory(struct gpio_mmio *gpio,
                                uint32_t address,
                                void *buffer,
                                size_t length,
                                size_t *bytes_read);

/* Read a posted MEM-AP DRW stream after TAR has already been selected. This
 * is the fast backend used by CMSIS-DAP TransferBlock. */
bool gpio_mmio_fast_read_block(struct gpio_mmio *gpio,
                               void *buffer,
                               size_t length,
                               size_t *bytes_read);

/* Write a sequential MEM-AP DRW stream after selecting TAR. The caller must
 * provide a word-aligned address and length; TAR must be reprogrammed by the
 * caller at each 1 KiB MEM-AP boundary. */
bool gpio_mmio_fast_write_memory(struct gpio_mmio *gpio,
                                 uint32_t address,
                                 const void *buffer,
                                 size_t length,
                                 size_t *bytes_written);

#endif
