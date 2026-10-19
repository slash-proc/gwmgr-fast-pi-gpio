#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/ioctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum {
    page_size = 4096,
    gpio_fsel1 = 0x04,
    gpio_set0 = 0x1c,
    gpio_clr0 = 0x28,
    gpio_lev0 = 0x34,
    dma_base = 0x007000,
    dma_channel = 5,
    max_control_blocks = 131072,
    max_samples = 16384,
    dma_cb_no_wide_bursts = 1U << 26,
    dma_cb_wait_resp = 1U << 3,
    dma_cb_dest_inc = 1U << 4,
    dma_cb_dest_dreq = 1U << 6,
    dma_cb_src_dreq = 1U << 10,
    dma_cs_active = 1U << 0,
    pwm_ctl = 0x00 / 4,
    pwm_dmac = 0x08 / 4,
    pwm_rng1 = 0x10 / 4,
    pwm_fif1 = 0x18 / 4,
    clock_pwmctl = 0xa0 / 4,
    clock_pwmdiv = 0xa4 / 4,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define GPIO_BUS_BASE UINT32_C(0x7e200000)
#define DMA_CS_RESET (UINT32_C(1) << 31)
#define DMA_CS_ABORT (UINT32_C(1) << 30)
#define DMA_CS_DISDEBUG (UINT32_C(1) << 28)
#define PWM_CTL_CLEAR_FIFO (UINT32_C(1) << 6)
#define PWM_CTL_MODE1 (UINT32_C(1) << 1)
#define PWM_CTL_USEF1 (UINT32_C(1) << 5)
#define PWM_CTL_PWEN1 UINT32_C(1)
#define PWM_DMAC_ENABLE (UINT32_C(1) << 31)
#define PWM_DMAC_PANIC_THRESHOLD (UINT32_C(7) << 8)
#define PWM_DMAC_DREQ_THRESHOLD UINT32_C(1)
#define CM_PASSWORD UINT32_C(0x5a000000)
#define CM_CTL_BUSY (UINT32_C(1) << 7)
#define CM_CTL_KILL (UINT32_C(1) << 5)
#define CM_CTL_ENABLE (UINT32_C(1) << 4)
#define CM_CTL_PLLD_PER UINT32_C(6)
#define DMA_TI_PERMAP(value) ((uint32_t)(value) << 16U)
#define PWM_BUS_BASE UINT32_C(0x7e20c000)
#define CLOCK_MANAGER_BASE (PERIPHERAL_BASE + UINT32_C(0x00101000))
#define PWM_BASE (PERIPHERAL_BASE + UINT32_C(0x0020c000))
#define PLLD_PER_HZ 750000000UL

struct dma_cb {
    uint32_t info;
    uint32_t source;
    uint32_t destination;
    uint32_t transfer_length;
    uint32_t stride;
    uint32_t next;
    uint32_t reserved[2];
};

struct vcsm_alloc {
    uint32_t size;
    uint32_t num;
    uint32_t cached;
    uint32_t pad;
    uint8_t name[32];
    int32_t handle;
    uint32_t vc_handle;
    uint64_t dma_address;
};

struct vcsm_block {
    void *mapping;
    size_t size;
    uint32_t bus_address;
    int allocation_fd;
};

struct dma_program {
    struct vcsm_block block;
    struct dma_cb *cbs;
    uint32_t *data;
    uint32_t *samples;
    size_t data_offset;
    size_t samples_offset;
    size_t cb_count;
    size_t data_count;
    size_t sample_count;
    uint32_t gpio_cb_info;
    bool swdio_high;
    bool swclk_high;
    uint32_t fsel_output;
    uint32_t fsel_input;
    bool paced;
    uint32_t pwm_fifo_bus;
    uint32_t sample_cb_info;
    unsigned pacer_feed_batch;
    unsigned pacer_tokens;
    size_t *memory_read_starts;
    size_t memory_read_count;
};

#define VC_SM_CMA_IOCTL_MEM_ALLOC _IOR('J', 0x5a, struct vcsm_alloc)

static volatile uint32_t *map_registers(uint32_t physical_address,
                                        size_t size,
                                        int *fd_out)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        return NULL;
    }
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, physical_address);
    if (mapping == MAP_FAILED) {
        (void)close(fd);
        return NULL;
    }
    *fd_out = fd;
    return (volatile uint32_t *)mapping;
}

static void unmap_registers(volatile uint32_t *mapping, size_t size, int fd)
{
    (void)munmap((void *)mapping, size);
    (void)close(fd);
}

static bool wait_clock_idle(volatile uint32_t *clock)
{
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        if ((clock[clock_pwmctl] & CM_CTL_BUSY) == 0U) {
            return true;
        }
        usleep(1U);
    }
    return false;
}

static bool configure_pacer(volatile uint32_t *clock,
                            volatile uint32_t *pwm,
                            unsigned long requested_hz,
                            unsigned long *actual_hz)
{
    if (requested_hz == 0UL || requested_hz > PLLD_PER_HZ) {
        return false;
    }
    unsigned long divisor = (PLLD_PER_HZ + requested_hz / 2UL) /
                            requested_hz;
    if (divisor < 1UL) {
        divisor = 1UL;
    }
    if (divisor > 4095UL) {
        divisor = 4095UL;
    }
    clock[clock_pwmctl] = CM_PASSWORD | CM_CTL_KILL;
    if (!wait_clock_idle(clock)) {
        return false;
    }
    clock[clock_pwmdiv] = CM_PASSWORD | ((uint32_t)divisor << 12U);
    clock[clock_pwmctl] = CM_PASSWORD | CM_CTL_PLLD_PER | CM_CTL_ENABLE;
    pwm[pwm_ctl] = 0U;
    pwm[pwm_dmac] = 0U;
    pwm[pwm_ctl] = PWM_CTL_CLEAR_FIFO;
    pwm[pwm_rng1] = 1U;
    pwm[pwm_dmac] = PWM_DMAC_ENABLE |
                    PWM_DMAC_PANIC_THRESHOLD |
                    PWM_DMAC_DREQ_THRESHOLD;
    /* Seed the FIFO before enabling PWM so the first DREQ is well-defined. */
    pwm[pwm_fif1] = 0U;
    pwm[pwm_ctl] = PWM_CTL_MODE1 | PWM_CTL_USEF1 | PWM_CTL_PWEN1;
    *actual_hz = PLLD_PER_HZ / divisor;
    return true;
}

static void stop_pacer(volatile uint32_t *clock, volatile uint32_t *pwm)
{
    pwm[pwm_ctl] = 0U;
    pwm[pwm_dmac] = 0U;
    clock[clock_pwmctl] = CM_PASSWORD | CM_CTL_KILL;
    (void)wait_clock_idle(clock);
}

static bool vcsm_block_alloc(size_t size, struct vcsm_block *block)
{
    size_t rounded = (size + page_size - 1U) & ~(size_t)(page_size - 1U);
    int vcsm_fd = open("/dev/vcsm-cma", O_RDWR | O_CLOEXEC);
    if (vcsm_fd < 0) {
        return false;
    }
    struct vcsm_alloc allocation = {
        .size = (uint32_t)rounded,
        .num = 1U,
        .cached = 0U,
        .handle = -1,
    };
    memcpy(allocation.name, "gwmgr-fast-pi-gpio-swd", sizeof("gwmgr-fast-pi-gpio-swd"));
    if (ioctl(vcsm_fd, VC_SM_CMA_IOCTL_MEM_ALLOC, &allocation) < 0 ||
        allocation.handle < 0 || allocation.dma_address > UINT32_MAX) {
        (void)close(vcsm_fd);
        return false;
    }
    void *mapping = mmap(NULL,
                         rounded,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         allocation.handle,
                         0);
    (void)close(vcsm_fd);
    if (mapping == MAP_FAILED) {
        (void)close(allocation.handle);
        return false;
    }
    block->mapping = mapping;
    block->size = rounded;
    block->bus_address = (uint32_t)allocation.dma_address;
    block->allocation_fd = allocation.handle;
    memset(mapping, 0, rounded);
    return true;
}

static void vcsm_block_free(struct vcsm_block *block)
{
    if (block->mapping != NULL) {
        (void)munmap(block->mapping, block->size);
        (void)close(block->allocation_fd);
        block->mapping = NULL;
    }
}

static uint32_t gpio_bus(uint32_t offset)
{
    return GPIO_BUS_BASE + offset;
}

static bool append_cb(struct dma_program *program,
                      uint32_t source,
                      uint32_t destination,
                      uint32_t info,
                      uint32_t transfer_length)
{
    if (program->cb_count >= max_control_blocks) {
        return false;
    }
    size_t index = program->cb_count++;
    struct dma_cb *cb = &program->cbs[index];
    cb->info = info;
    cb->source = source;
    cb->destination = destination;
    cb->transfer_length = transfer_length;
    cb->stride = 0U;
    cb->next = 0U;
    cb->reserved[0] = 0U;
    cb->reserved[1] = 0U;
    if (index != 0U) {
        program->cbs[index - 1U].next =
            program->block.bus_address + (uint32_t)(index * sizeof(struct dma_cb));
    }
    return true;
}

static bool append_pacer_feed(struct dma_program *program)
{
    if (!program->paced || program->pacer_tokens != 0U) {
        return true;
    }
    size_t feed_data_index = program->data_count++;
    program->data[feed_data_index] = 0U;
    if (!append_cb(program,
                   program->block.bus_address +
                       (uint32_t)(program->data_offset +
                                  feed_data_index * sizeof(uint32_t)),
                   program->pwm_fifo_bus,
                   dma_cb_no_wide_bursts | dma_cb_wait_resp,
                   (uint32_t)(program->pacer_feed_batch * sizeof(uint32_t)))) {
        return false;
    }
    program->pacer_tokens = program->pacer_feed_batch;
    return true;
}

static bool append_gpio_write(struct dma_program *program,
                              uint32_t destination,
                              uint32_t value)
{
    if (!append_pacer_feed(program)) {
        return false;
    }
    size_t data_index = program->data_count++;
    program->data[data_index] = value;
    if (!append_cb(program,
                   program->block.bus_address +
                       (uint32_t)(program->data_offset +
                                  data_index * sizeof(uint32_t)),
                   destination,
                   program->gpio_cb_info,
                   sizeof(uint32_t))) {
        return false;
    }
    if (program->paced) {
        program->pacer_tokens--;
    }
    return true;
}

static bool append_fsel(struct dma_program *program, uint32_t value)
{
    return append_gpio_write(program, gpio_bus(gpio_fsel1), value);
}

static bool append_sample(struct dma_program *program)
{
    if (program->sample_count >= max_samples) {
        return false;
    }
    if (!append_pacer_feed(program)) {
        return false;
    }
    uint32_t destination = program->block.bus_address +
                           (uint32_t)(program->samples_offset +
                                      program->sample_count * sizeof(uint32_t));
    program->sample_count++;
    if (!append_cb(program,
                   gpio_bus(gpio_lev0),
                   destination,
                   program->sample_cb_info,
                   sizeof(uint32_t))) {
        return false;
    }
    if (!program->paced) {
        return true;
    }
    /* Samples and GPIO writes share the same PWM token pool. A sample
     * consumes one token just like a paced GPIO write; refill it before the
     * next paced operation when the current batch is exhausted. */
    program->pacer_tokens--;
    return true;
}

static bool append_output_cycle(struct dma_program *program, bool bit)
{
    uint32_t swdio_mask = UINT32_C(1) << 10;
    uint32_t swclk_mask = UINT32_C(1) << 11;
    bool swdio_changes = program->swdio_high != bit;
    if (program->swclk_high) {
        /* A falling edge and a transition to SWDIO low can share one
         * GPCLR write. The data then remains stable until the GPSET edge. */
        uint32_t clear_mask = swclk_mask;
        if (swdio_changes && !bit) {
            clear_mask |= swdio_mask;
            program->swdio_high = false;
            swdio_changes = false;
        }
        if (!append_gpio_write(program, gpio_bus(gpio_clr0), clear_mask)) {
            return false;
        }
        program->swclk_high = false;
    }
    if (swdio_changes) {
        if (!append_gpio_write(program,
                               gpio_bus(bit ? gpio_set0 : gpio_clr0),
                               swdio_mask)) {
            return false;
        }
        program->swdio_high = bit;
    }
    if (!append_gpio_write(program, gpio_bus(gpio_set0), swclk_mask)) {
        return false;
    }
    program->swclk_high = true;
    return true;
}

static bool append_output_rise(struct dma_program *program, bool bit)
{
    return append_output_cycle(program, bit);
}

static bool append_input_cycle(struct dma_program *program, bool sample)
{
    uint32_t swclk_mask = UINT32_C(1) << 11;
    if (program->swclk_high &&
        !append_gpio_write(program, gpio_bus(gpio_clr0), swclk_mask)) {
        return false;
    }
    if (sample && !append_sample(program)) {
        return false;
    }
    if (!append_gpio_write(program, gpio_bus(gpio_set0), swclk_mask)) {
        return false;
    }
    program->swclk_high = true;
    return true;
}

static bool append_line_reset(struct dma_program *program)
{
    for (unsigned count = 0U; count < 56U; ++count) {
        if (!append_output_cycle(program, true)) {
            return false;
        }
    }
    return true;
}

static bool append_request_byte(struct dma_program *program, uint8_t request)
{
    for (unsigned bit = 0U; bit < 8U; ++bit) {
        bool last = bit == 7U;
        bool bit_value = ((request >> bit) & 1U) != 0U;
        if (!(last ? append_output_rise(program, bit_value)
                   : append_output_cycle(program, bit_value))) {
            return false;
        }
    }
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

static bool append_request(struct dma_program *program,
                           bool ap,
                           bool read,
                           uint8_t address)
{
    return append_request_byte(program, encode_request(ap, read, address));
}

static bool append_swj_sequence(struct dma_program *program)
{
    const uint16_t sequence = 0xe79eU;
    for (unsigned bit = 0U; bit < 16U; ++bit) {
        if (!append_output_cycle(program, ((sequence >> bit) & 1U) != 0U)) {
            return false;
        }
    }
    return true;
}

static bool odd_parity32(uint32_t value)
{
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        ones += (unsigned)((value >> bit) & 1U);
    }
    return (ones & 1U) != 0U;
}

static bool append_write_transfer(struct dma_program *program,
                                  bool ap,
                                  uint8_t address,
                                  uint32_t value)
{
    if (!append_request(program, ap, false, address) ||
        !append_fsel(program, program->fsel_input) ||
        !append_input_cycle(program, false)) {
        return false;
    }
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (!append_input_cycle(program, true)) {
            return false;
        }
    }
    if (!append_input_cycle(program, false) ||
        !append_fsel(program, program->fsel_output) ||
        !append_gpio_write(program, gpio_bus(gpio_set0), 1U << 10U)) {
        return false;
    }
    program->swdio_high = true;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (!append_output_cycle(program, ((value >> bit) & 1U) != 0U)) {
            return false;
        }
    }
    return append_output_cycle(program, odd_parity32(value));
}

static bool append_read_transfer(struct dma_program *program,
                                 bool ap,
                                 uint8_t address,
                                 size_t *sample_start)
{
    if (!append_request(program, ap, true, address) ||
        !append_fsel(program, program->fsel_input) ||
        !append_input_cycle(program, false)) {
        return false;
    }
    if (sample_start != NULL) {
        *sample_start = program->sample_count;
    }
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (!append_input_cycle(program, true)) {
            return false;
        }
    }
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (!append_input_cycle(program, true)) {
            return false;
        }
    }
    if (!append_input_cycle(program, true) ||
        !append_input_cycle(program, false) ||
        !append_fsel(program, program->fsel_output) ||
        !append_gpio_write(program, gpio_bus(gpio_set0), 1U << 10U)) {
        return false;
    }
    program->swdio_high = true;
    return true;
}

static bool build_idcode_program(struct dma_program *program,
                                 uint32_t fsel_base)
{
    uint32_t fsel_output = fsel_base | (1U << 0U) | (1U << 3U);
    uint32_t fsel_input = fsel_base | (1U << 3U);
    uint32_t both_low = (1U << 10U) | (1U << 11U);
    program->fsel_output = fsel_output;
    program->fsel_input = fsel_input;

    if (!append_fsel(program, fsel_output) ||
        !append_gpio_write(program, gpio_bus(gpio_clr0), both_low) ||
        !append_line_reset(program) ||
        !append_swj_sequence(program) ||
        !append_line_reset(program) ||
        !append_output_cycle(program, false) ||
        !append_output_cycle(program, false) ||
        !append_request(program, false, true, 0U) ||
        !append_fsel(program, fsel_input) ||
        !append_input_cycle(program, false)) {
        return false;
    }

    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if (!append_input_cycle(program, true)) {
            return false;
        }
    }
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (!append_input_cycle(program, true)) {
            return false;
        }
    }
    if (!append_input_cycle(program, true) ||
        !append_input_cycle(program, false) ||
        !append_fsel(program, fsel_output) ||
        !append_gpio_write(program, gpio_bus(gpio_set0), 1U << 10U)) {
        return false;
    }
    return append_input_cycle(program, false);
}

static bool build_memory_program(struct dma_program *program,
                                 uint32_t fsel_base,
                                 uint32_t address,
                                 size_t words,
                                 unsigned post_idle_cycles)
{
    if (words == 0U || !build_idcode_program(program, fsel_base)) {
        return false;
    }

    /* Probe whether a second back-to-back DP read is accepted before adding
     * the MEM-AP setup sequence. This is temporary transaction-boundary
     * instrumentation for the DMA waveform. */
    for (unsigned count = 0U; count < post_idle_cycles; ++count) {
        if (!append_output_cycle(program, true)) {
            return false;
        }
    }
    if (!append_output_cycle(program, false) ||
        !append_output_cycle(program, false) ||
        !append_read_transfer(program, false, 0U, NULL)) {
        return false;
    }

    /* Prepare the DP and MEM-AP in the same order as the CPU baseline. */
    if (!append_write_transfer(program, false, 0U, 0x0000001eU) ||
        !append_write_transfer(program, false, 4U, 0x50000000U)) {
        return false;
    }
    for (unsigned attempt = 0U; attempt < 4U; ++attempt) {
        if (!append_read_transfer(program, false, 4U, NULL)) {
            return false;
        }
    }
    if (!append_write_transfer(program, false, 8U, 0U)) {
        return false;
    }
    if (!append_write_transfer(program, true, 0U, 0x23000052U) ||
        !append_write_transfer(program, true, 4U, address)) {
        return false;
    }

    size_t discard_start = 0U;
    if (!append_read_transfer(program, true, 12U, &discard_start)) {
        return false;
    }
    (void)discard_start;

    program->memory_read_count = words;
    for (size_t word = 0U; word + 1U < words; ++word) {
        if (!append_read_transfer(program,
                                  true,
                                  12U,
                                  &program->memory_read_starts[word])) {
            return false;
        }
    }
    return append_read_transfer(program,
                                false,
                                12U,
                                &program->memory_read_starts[words - 1U]);
}

static bool parse_result(const struct dma_program *program, uint32_t *idcode)
{
    if (program->sample_count != 36U) {
        return false;
    }
    uint32_t ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if ((program->samples[bit] & (1U << 10U)) != 0U) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        fprintf(stderr, "DMA SWD ACK=0x%x\n", ack);
        return false;
    }
    uint32_t value = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        bool high = (program->samples[3U + bit] & (1U << 10U)) != 0U;
        if (high) {
            value |= 1U << bit;
            ones++;
        }
    }
    bool parity = (program->samples[35U] & (1U << 10U)) != 0U;
    if (parity != ((ones & 1U) != 0U)) {
        fprintf(stderr, "DMA SWD parity failure\n");
        return false;
    }
    *idcode = value;
    return true;
}

static bool parse_read_response(const struct dma_program *program,
                                size_t start,
                                uint32_t *value)
{
    if (start + 36U > program->sample_count) {
        return false;
    }
    uint32_t ack = 0U;
    for (unsigned bit = 0U; bit < 3U; ++bit) {
        if ((program->samples[start + bit] & (1U << 10U)) != 0U) {
            ack |= 1U << bit;
        }
    }
    if (ack != 1U) {
        return false;
    }
    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        bool high = (program->samples[start + 3U + bit] & (1U << 10U)) != 0U;
        if (high) {
            result |= 1U << bit;
            ones++;
        }
    }
    bool parity = (program->samples[start + 35U] & (1U << 10U)) != 0U;
    if (parity != ((ones & 1U) != 0U)) {
        return false;
    }
    *value = result;
    return true;
}

static bool parse_memory_result(const struct dma_program *program,
                                uint32_t *words)
{
    static const size_t diagnostic_starts[] = {
        36U, 72U, 75U, 78U, 114U, 150U, 186U, 222U, 225U, 228U, 267U
    };
    for (size_t index = 0U;
         index < sizeof(diagnostic_starts) / sizeof(diagnostic_starts[0]);
         ++index) {
        size_t start = diagnostic_starts[index];
        if (start + 3U > program->sample_count) {
            break;
        }
        unsigned ack = 0U;
        for (unsigned bit = 0U; bit < 3U; ++bit) {
            if ((program->samples[start + bit] & (1U << 10U)) != 0U) {
                ack |= 1U << bit;
            }
        }
        fprintf(stderr, "DMA SWD diagnostic sample %zu ACK=0x%x\n", start, ack);
    }
    for (size_t index = 0U; index < program->memory_read_count; ++index) {
        if (!parse_read_response(program,
                                 program->memory_read_starts[index],
                                 &words[index])) {
            size_t start = program->memory_read_starts[index];
            unsigned ack = 0U;
            for (unsigned bit = 0U; bit < 3U; ++bit) {
                if ((program->samples[start + bit] & (1U << 10U)) != 0U) {
                    ack |= 1U << bit;
                }
            }
            fprintf(stderr,
                    "DMA SWD memory response %zu failed at sample %zu ACK=0x%x\n",
                    index,
                    start,
                    ack);
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

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--address HEX --words COUNT [--output FILE] "
            "[--idle COUNT] [--no-wait] [--pacer HZ] [--feed-batch COUNT]]\n",
            program);
}

int main(int argc, char **argv)
{
    uint32_t address = 0x24000000U;
    size_t words = 0U;
    unsigned post_idle_cycles = 0U;
    bool wait_for_gpio_writes = true;
    unsigned long pacer_hz = 0UL;
    unsigned pacer_feed_batch = 8U;
    const char *output_path = NULL;
    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--address") == 0 &&
            index + 1 < argc &&
            parse_unsigned(argv[++index], &value)) {
            address = (uint32_t)value;
            words = 1U;
        } else if (strcmp(argv[index], "--words") == 0 &&
                   index + 1 < argc &&
                   parse_unsigned(argv[++index], &value)) {
            words = (size_t)value;
        } else if (strcmp(argv[index], "--output") == 0 &&
                   index + 1 < argc) {
            output_path = argv[++index];
        } else if (strcmp(argv[index], "--idle") == 0 &&
                   index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) &&
                   value <= UINT_MAX) {
            post_idle_cycles = (unsigned)value;
        } else if (strcmp(argv[index], "--no-wait") == 0) {
            wait_for_gpio_writes = false;
        } else if (strcmp(argv[index], "--pacer") == 0 &&
                   index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) && value > 0UL) {
            pacer_hz = value;
        } else if (strcmp(argv[index], "--feed-batch") == 0 &&
                   index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) &&
                   value >= 1UL && value <= 16UL) {
            pacer_feed_batch = (unsigned)value;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (words > 256U) {
        fprintf(stderr, "words must be between 1 and 256\n");
        return EXIT_FAILURE;
    }
    bool memory_mode = words != 0U;

    int gpio_fd = -1;
    volatile uint32_t *gpio = map_registers(PERIPHERAL_BASE + 0x00200000U,
                                             page_size,
                                             &gpio_fd);
    if (gpio == NULL) {
        perror("GPIO map");
        return EXIT_FAILURE;
    }
    uint32_t original_fsel = gpio[gpio_fsel1 / sizeof(uint32_t)];
    size_t *memory_read_starts = memory_mode ? calloc(words, sizeof(size_t)) : NULL;
    uint32_t *memory_words = memory_mode ? calloc(words, sizeof(uint32_t)) : NULL;
    if (memory_mode && (memory_read_starts == NULL || memory_words == NULL)) {
        fprintf(stderr, "cannot allocate host-side read buffers\n");
        free(memory_read_starts);
        free(memory_words);
        unmap_registers(gpio, page_size, gpio_fd);
        return EXIT_FAILURE;
    }

    size_t cb_bytes = max_control_blocks * sizeof(struct dma_cb);
    size_t data_offset = (cb_bytes + 31U) & ~(size_t)31U;
    size_t samples_offset = (data_offset + max_control_blocks * sizeof(uint32_t) + 31U) &
                            ~(size_t)31U;
    size_t allocation_size = samples_offset + max_samples * sizeof(uint32_t);
    struct dma_program program = {
        .data_offset = data_offset,
        .samples_offset = samples_offset,
        .swdio_high = false,
        .swclk_high = false,
        .gpio_cb_info = dma_cb_no_wide_bursts |
                        (wait_for_gpio_writes ? dma_cb_wait_resp : 0U) |
                        (pacer_hz != 0UL
                             ? dma_cb_dest_dreq | DMA_TI_PERMAP(5U)
                             : 0U),
        .paced = pacer_hz != 0UL,
        .pwm_fifo_bus = PWM_BUS_BASE + UINT32_C(0x18),
        .pacer_feed_batch = pacer_feed_batch,
        .sample_cb_info = dma_cb_no_wide_bursts |
                          dma_cb_wait_resp |
                          dma_cb_dest_inc |
                          (pacer_hz != 0UL
                               ? dma_cb_src_dreq | DMA_TI_PERMAP(5U)
                               : 0U),
        .memory_read_starts = memory_read_starts,
    };
    if (!vcsm_block_alloc(allocation_size, &program.block)) {
        fprintf(stderr, "cannot allocate DMA-visible program memory\n");
        free(memory_read_starts);
        free(memory_words);
        unmap_registers(gpio, page_size, gpio_fd);
        return EXIT_FAILURE;
    }
    program.cbs = (struct dma_cb *)program.block.mapping;
    program.data = (uint32_t *)((uint8_t *)program.block.mapping + data_offset);
    program.samples = (uint32_t *)((uint8_t *)program.block.mapping + samples_offset);

    uint32_t fsel_base = original_fsel & ~((7U << 0U) | (7U << 3U));
    bool built = memory_mode
                     ? build_memory_program(&program,
                                            fsel_base,
                                            address,
                                            words,
                                            post_idle_cycles)
                     : build_idcode_program(&program, fsel_base);
    if (!built) {
        fprintf(stderr, "cannot build DMA SWD program (%zu control blocks)\n",
                program.cb_count);
        vcsm_block_free(&program.block);
        free(memory_read_starts);
        free(memory_words);
        unmap_registers(gpio, page_size, gpio_fd);
        return EXIT_FAILURE;
    }

    int dma_fd = -1;
    volatile uint32_t *dma_registers = map_registers(PERIPHERAL_BASE + dma_base,
                                                     page_size,
                                                     &dma_fd);
    if (dma_registers == NULL) {
        perror("DMA map");
        vcsm_block_free(&program.block);
        free(memory_read_starts);
        free(memory_words);
        unmap_registers(gpio, page_size, gpio_fd);
        return EXIT_FAILURE;
    }
    int pwm_fd = -1;
    int clock_fd = -1;
    volatile uint32_t *pwm = NULL;
    volatile uint32_t *clock = NULL;
    unsigned long actual_pacer_hz = 0UL;
    if (pacer_hz != 0UL) {
        pwm = map_registers(PWM_BASE, page_size, &pwm_fd);
        clock = map_registers(CLOCK_MANAGER_BASE,
                              page_size,
                              &clock_fd);
        if (pwm == NULL || clock == NULL ||
            !configure_pacer(clock, pwm, pacer_hz, &actual_pacer_hz)) {
            fprintf(stderr, "cannot configure PWM pacer\n");
            if (clock != NULL) {
                unmap_registers(clock, page_size, clock_fd);
            }
            if (pwm != NULL) {
                unmap_registers(pwm, page_size, pwm_fd);
            }
            unmap_registers(dma_registers, page_size, dma_fd);
            vcsm_block_free(&program.block);
            free(memory_read_starts);
            free(memory_words);
            unmap_registers(gpio, page_size, gpio_fd);
            return EXIT_FAILURE;
        }
    }
    volatile uint32_t *dma =
        dma_registers + (dma_channel * 0x100U / sizeof(uint32_t));
    dma[0] = DMA_CS_ABORT;
    usleep(100U);
    dma[0] = DMA_CS_RESET;
    dma[1] = program.block.bus_address;
    __sync_synchronize();
    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    dma[0] = (7U << 16) | (7U << 20) | DMA_CS_DISDEBUG | dma_cs_active;

    bool complete = false;
    /* Poll the channel directly so the elapsed time is the DMA time rather
     * than a multiple of the scheduler's sleep interval. The periodic clock
     * check still gives a bounded failure path if the channel wedges. */
    for (unsigned attempt = 0U; attempt < 50000000U; ++attempt) {
        if ((dma[0] & dma_cs_active) == 0U) {
            complete = true;
            break;
        }
        if ((attempt & 0xffffU) == 0U) {
            struct timespec now = {0};
            (void)clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            if (elapsed_seconds(&start, &now) > 5.0) {
                break;
            }
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    uint32_t status = dma[0];
    dma[0] = DMA_CS_ABORT;
    usleep(100U);
    dma[0] = DMA_CS_RESET;
    if (pacer_hz != 0UL) {
        stop_pacer(clock, pwm);
    }
    if (clock != NULL) {
        unmap_registers(clock, page_size, clock_fd);
    }
    if (pwm != NULL) {
        unmap_registers(pwm, page_size, pwm_fd);
    }
    unmap_registers(dma_registers, page_size, dma_fd);
    gpio[gpio_fsel1 / sizeof(uint32_t)] = original_fsel;
    unmap_registers(gpio, page_size, gpio_fd);

    if (!complete) {
        fprintf(stderr, "DMA SWD timeout, status=0x%08" PRIx32 "\n", status);
        vcsm_block_free(&program.block);
        free(memory_read_starts);
        free(memory_words);
        return EXIT_FAILURE;
    }

    uint32_t idcode = 0U;
    bool valid = memory_mode
                     ? parse_read_response(&program, 0U, &idcode)
                     : parse_result(&program, &idcode);
    if (valid && memory_mode) {
        valid = parse_memory_result(&program, memory_words);
    }
    double seconds = elapsed_seconds(&start, &end);
    if (valid && memory_mode && output_path != NULL) {
        FILE *output = fopen(output_path, "wb");
        if (output == NULL ||
            fwrite(memory_words, sizeof(uint32_t), words, output) != words) {
            fprintf(stderr, "cannot write output file '%s'\n", output_path);
            valid = false;
        }
        if (output != NULL) {
            (void)fclose(output);
        }
    }
    if (valid && memory_mode) {
        printf("DMA SWD IDCODE: 0x%08" PRIx32 "\n", idcode);
        printf("DMA SWD read: %zu bytes in %.6f s (%.1f KiB/s, %zu CBs, %zu samples",
               words * sizeof(uint32_t),
               seconds,
               ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
               program.cb_count,
               program.sample_count);
        if (pacer_hz != 0UL) {
            printf(", pacer=%lu Hz", actual_pacer_hz);
        }
        printf(")\n");
    } else {
        printf("DMA SWD IDCODE: 0x%08" PRIx32 " (%zu CBs, %zu samples)\n",
               idcode,
               program.cb_count,
               program.sample_count);
    }
    vcsm_block_free(&program.block);
    free(memory_read_starts);
    free(memory_words);
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
