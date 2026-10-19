#define _GNU_SOURCE

/*
 * Experimental streamed DMA reader.
 *
 * The older DMA proof allocates a control block for every GPIO write and
 * every input sample.  This prototype moves the samples to a second DMA
 * channel and loops a fixed AP-read waveform on the output channel.  PWM
 * DREQ paces the output and sample channels while the output ring feeds the
 * PWM FIFO between GPIO writes. The output ring
 * is independent of the number of words, so the control-block count no
 * longer grows with the transfer size.
 */

#include "gpio_mmio.h"
#include "swd.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
    gpio_fsel1 = 0x04 / 4,
    gpio_set0 = 0x1c / 4,
    gpio_clr0 = 0x28 / 4,
    gpio_lev0 = 0x34 / 4,
    pwm_ctl = 0x00 / 4,
    pwm_dmac = 0x08 / 4,
    pwm_rng1 = 0x10 / 4,
    pwm_fif1 = 0x18 / 4,
    clock_pwmctl = 0xa0 / 4,
    clock_pwmdiv = 0xa4 / 4,
    dma_base = 0x007000,
    output_dma_channel = 5,
    sample_dma_channel = 6,
    max_output_cbs = 384,
    max_sample_words_per_cb = 1000000,
    dma_cs_active = 1U << 0,
    dma_cb_wait_resp = 1U << 3,
    dma_cb_dest_inc = 1U << 4,
    dma_cb_dest_dreq = 1U << 6,
    dma_cb_src_inc = 1U << 8,
    dma_cb_src_dreq = 1U << 10,
    dma_cb_no_wide_bursts = 1U << 26,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define GPIO_BUS_BASE UINT32_C(0x7e200000)
#define PWM_BASE (PERIPHERAL_BASE + UINT32_C(0x0020c000))
#define PWM_BUS_BASE UINT32_C(0x7e20c000)
#define PWM_SAMPLE_BASE (PERIPHERAL_BASE + UINT32_C(0x0020c800))
#define PWM_SAMPLE_BUS_BASE UINT32_C(0x7e20c800)
#define CLOCK_MANAGER_BASE (PERIPHERAL_BASE + UINT32_C(0x00101000))
#define DMA_CS_RESET (UINT32_C(1) << 31)
#define DMA_CS_ABORT (UINT32_C(1) << 30)
#define DMA_CS_DISDEBUG (UINT32_C(1) << 28)
#define DMA_CS_PRIORITY(value) ((uint32_t)(value) << 16U)
#define DMA_CS_PANIC_PRIORITY(value) ((uint32_t)(value) << 20U)
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

struct ring_program {
    uint32_t destinations[max_output_cbs];
    uint32_t values[max_output_cbs];
    bool paced[max_output_cbs];
    size_t count;
    size_t dreq_count;
    size_t clear_sample_index[36];
    size_t set_sample_index[36];
    bool swdio_high;
    bool swclk_high;
};

static volatile uint32_t *map_registers(uint32_t address,
                                        size_t size,
                                        int *fd_out)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        return NULL;
    }
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, address);
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

static bool vcsm_block_alloc(size_t size, struct vcsm_block *block)
{
    size_t rounded = (size + page_size - 1U) & ~(size_t)(page_size - 1U);
    int fd = open("/dev/vcsm-cma", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct vcsm_alloc allocation = {
        .size = (uint32_t)rounded,
        .num = 1U,
        .cached = 0U,
        .handle = -1,
    };
    memcpy(allocation.name, "gwmgr-fast-pi-gpio-stream", sizeof("gwmgr-fast-pi-gpio-stream"));
    if (ioctl(fd, _IOR('J', 0x5a, struct vcsm_alloc), &allocation) < 0 ||
        allocation.handle < 0 || allocation.dma_address > UINT32_MAX) {
        (void)close(fd);
        return false;
    }
    void *mapping = mmap(NULL,
                         rounded,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         allocation.handle,
                         0);
    (void)close(fd);
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

static size_t align32(size_t value)
{
    return (value + 31U) & ~(size_t)31U;
}

static bool ring_append_raw(struct ring_program *program,
                            uint32_t destination,
                            uint32_t value,
                            bool paced,
                            size_t *index_out)
{
    if (program->count >= max_output_cbs) {
        return false;
    }
    size_t index = program->count++;
    program->destinations[index] = destination;
    program->values[index] = value;
    program->paced[index] = paced;
    if (index_out != NULL) {
        *index_out = index;
    }
    return true;
}

static bool ring_gpio_write(struct ring_program *program,
                            uint32_t destination,
                            uint32_t value,
                            size_t *index_out)
{
    size_t sample_index = program->dreq_count;
    if (!ring_append_raw(program, destination, value, true, NULL)) {
        return false;
    }
    program->dreq_count++;
    if (index_out != NULL) {
        *index_out = sample_index;
    }
    /* Keep one word in the PWM FIFO so its DREQ remains a real timebase. */
    if (!ring_append_raw(program,
                         PWM_BUS_BASE + UINT32_C(0x18),
                         0U,
                         false,
                         NULL)) {
        return false;
    }
    return ring_append_raw(program,
                           PWM_SAMPLE_BUS_BASE + UINT32_C(0x18),
                           0U,
                           false,
                           NULL);
}

static bool ring_fsel(struct ring_program *program, uint32_t value)
{
    return ring_gpio_write(program,
                           GPIO_BUS_BASE + UINT32_C(0x04),
                           value,
                           NULL);
}

static bool ring_output_cycle(struct ring_program *program, bool bit)
{
    uint32_t swclk_mask = UINT32_C(1) << 11U;
    uint32_t swdio_mask = UINT32_C(1) << 10U;
    bool data_changes = program->swdio_high != bit;
    if (program->swclk_high) {
        uint32_t clear_mask = swclk_mask;
        if (data_changes && !bit) {
            clear_mask |= swdio_mask;
            program->swdio_high = false;
            data_changes = false;
        }
        if (!ring_gpio_write(program,
                             GPIO_BUS_BASE + UINT32_C(0x28),
                             clear_mask,
                             NULL)) {
            return false;
        }
        program->swclk_high = false;
    }
    if (data_changes) {
        if (!ring_gpio_write(program,
                             GPIO_BUS_BASE + (bit ? UINT32_C(0x1c)
                                                  : UINT32_C(0x28)),
                             swdio_mask,
                             NULL)) {
            return false;
        }
        program->swdio_high = bit;
    }
    if (!ring_gpio_write(program,
                         GPIO_BUS_BASE + UINT32_C(0x1c),
                         swclk_mask,
                         NULL)) {
        return false;
    }
    program->swclk_high = true;
    return true;
}

static bool ring_input_cycle(struct ring_program *program,
                             size_t *clear_index,
                             size_t *set_index)
{
    uint32_t swclk_mask = UINT32_C(1) << 11U;
    if (program->swclk_high &&
        !ring_gpio_write(program,
                         GPIO_BUS_BASE + UINT32_C(0x28),
                         swclk_mask,
                         clear_index)) {
        return false;
    }
    if (!ring_gpio_write(program,
                         GPIO_BUS_BASE + UINT32_C(0x1c),
                         swclk_mask,
                         set_index)) {
        return false;
    }
    program->swclk_high = true;
    return true;
}

static bool build_ring(struct ring_program *program,
                       uint32_t fsel_base,
                       bool dp_idcode)
{
    uint8_t request = swd_encode_request(
        (struct swd_request){.ap = !dp_idcode, .read = true, .address = dp_idcode ? 0U : 12U});
    uint32_t fsel_input = fsel_base | (1U << 3U);
    uint32_t fsel_output = fsel_base | (1U << 0U) | (1U << 3U);
    program->swdio_high = true;
    program->swclk_high = true;
    /* Both PWM FIFOs must contain a first pacing word before the first
     * destination-DREQ GPIO write can run. These entries are executed once;
     * the repeating ring starts at entry 2. */
    if (!ring_append_raw(program,
                         PWM_BUS_BASE + UINT32_C(0x18),
                         0U,
                         false,
                         NULL) ||
        !ring_append_raw(program,
                         PWM_SAMPLE_BUS_BASE + UINT32_C(0x18),
                         0U,
                         false,
                         NULL)) {
        return false;
    }

    for (unsigned bit = 0U; bit < 8U; ++bit) {
        if (!ring_output_cycle(program,
                               ((request >> bit) & 1U) != 0U)) {
            return false;
        }
    }
    if (!ring_fsel(program, fsel_input)) {
        return false;
    }
    if (!ring_input_cycle(program, NULL, NULL)) {
        return false;
    }
    for (unsigned bit = 0U; bit < 36U; ++bit) {
        if (!ring_input_cycle(program,
                              &program->clear_sample_index[bit],
                              &program->set_sample_index[bit])) {
            return false;
        }
    }
    if (!ring_fsel(program, fsel_output) ||
        !ring_gpio_write(program,
                         GPIO_BUS_BASE + UINT32_C(0x1c),
                         UINT32_C(1) << 10U,
                         NULL)) {
        return false;
    }
    return program->count != 0U;
}

static void initialize_cb(struct dma_cb *cb,
                          uint32_t source,
                          uint32_t destination,
                          uint32_t info,
                          uint32_t transfer_length,
                          uint32_t next)
{
    cb->info = info;
    cb->source = source;
    cb->destination = destination;
    cb->transfer_length = transfer_length;
    cb->stride = 0U;
    cb->next = next;
    cb->reserved[0] = 0U;
    cb->reserved[1] = 0U;
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
                            volatile uint32_t *sample_pwm,
                            unsigned long dreq_hz,
                            unsigned long *actual_hz)
{
    if (dreq_hz == 0UL || dreq_hz > PLLD_PER_HZ) {
        return false;
    }
    unsigned long divisor = (PLLD_PER_HZ + dreq_hz / 2UL) / dreq_hz;
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
    pwm[pwm_ctl] = PWM_CTL_MODE1 | PWM_CTL_USEF1 | PWM_CTL_PWEN1;
    sample_pwm[pwm_ctl] = 0U;
    sample_pwm[pwm_dmac] = 0U;
    sample_pwm[pwm_ctl] = PWM_CTL_CLEAR_FIFO;
    sample_pwm[pwm_rng1] = 1U;
    sample_pwm[pwm_dmac] = PWM_DMAC_ENABLE |
                           PWM_DMAC_PANIC_THRESHOLD |
                           PWM_DMAC_DREQ_THRESHOLD;
    sample_pwm[pwm_ctl] = PWM_CTL_MODE1 | PWM_CTL_USEF1 | PWM_CTL_PWEN1;
    *actual_hz = PLLD_PER_HZ / divisor;
    return true;
}

static void stop_pacer(volatile uint32_t *clock,
                       volatile uint32_t *pwm,
                       volatile uint32_t *sample_pwm)
{
    pwm[pwm_ctl] = 0U;
    pwm[pwm_dmac] = 0U;
    sample_pwm[pwm_ctl] = 0U;
    sample_pwm[pwm_dmac] = 0U;
    clock[clock_pwmctl] = CM_PASSWORD | CM_CTL_KILL;
    (void)wait_clock_idle(clock);
}

static void reset_dma_channel(volatile uint32_t *channel)
{
    channel[0] = DMA_CS_ABORT;
    usleep(10U);
    channel[0] = DMA_CS_RESET;
    usleep(10U);
}

static uint32_t sample_gpio_bit(uint32_t value)
{
    return (value & (UINT32_C(1) << 10U)) != 0U ? 1U : 0U;
}

static bool decode_response(const uint32_t *samples,
                            size_t sample_count,
                            size_t base,
                            const struct ring_program *ring,
                            bool use_set_samples,
                            uint32_t *value)
{
    uint32_t bits[36] = {0U};
    for (unsigned bit = 0U; bit < 36U; ++bit) {
        size_t index = base + (use_set_samples
                                   ? ring->set_sample_index[bit]
                                   : ring->clear_sample_index[bit]);
        if (index >= sample_count) {
            return false;
        }
        bits[bit] = sample_gpio_bit(samples[index]);
    }
    unsigned ack = bits[0] | (bits[1] << 1U) | (bits[2] << 2U);
    if (ack != 1U) {
        return false;
    }
    uint32_t result = 0U;
    unsigned ones = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit) {
        if (bits[3U + bit] != 0U) {
            result |= UINT32_C(1) << bit;
            ones++;
        }
    }
    if ((bits[35] != 0U) != ((ones & 1U) != 0U)) {
        return false;
    }
    *value = result;
    return true;
}

static bool parse_stream(const uint32_t *samples,
                         size_t sample_count,
                         const struct ring_program *ring,
                         size_t transactions,
                         uint32_t *words,
                         size_t *phase_out,
                         bool *set_out)
{
    for (size_t phase = 0U; phase < ring->dreq_count; ++phase) {
        for (unsigned use_set = 0U; use_set < 2U; ++use_set) {
            bool valid = true;
            for (size_t transaction = 0U;
                 transaction < transactions && valid;
                 ++transaction) {
                size_t base = phase + transaction * ring->dreq_count;
                uint32_t value = 0U;
                valid = decode_response(samples,
                                        sample_count,
                                        base,
                                        ring,
                                        use_set != 0U,
                                        &value);
                if (valid && transaction != 0U) {
                    words[transaction - 1U] = value;
                }
            }
            if (valid) {
                *phase_out = phase;
                *set_out = use_set != 0U;
                return true;
            }
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
    unsigned long dreq_hz = 20000000UL;
    bool dp_idcode = false;
    bool sample_destination_dreq = false;
    const char *output_path = NULL;
    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--address") == 0 && index + 1 < argc &&
            parse_unsigned(argv[++index], &value)) {
            address = (uint32_t)value;
        } else if (strcmp(argv[index], "--words") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) && value > 0UL &&
                   value <= 65536UL) {
            words = (size_t)value;
        } else if (strcmp(argv[index], "--dreq") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value) && value > 0UL) {
            dreq_hz = value;
        } else if (strcmp(argv[index], "--dp") == 0) {
            dp_idcode = true;
        } else if (strcmp(argv[index], "--sample-dest-dreq") == 0) {
            sample_destination_dreq = true;
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else {
            fprintf(stderr,
                    "Usage: %s [--address HEX] [--words COUNT] "
                             "[--dreq HZ] [--dp] [--sample-dest-dreq] "
                    "[--output FILE]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }

    struct gpio_mmio gpio = {0};
    if (gpio_mmio_open(&gpio, 11U, 10U, 6000000U) != 0) {
        fprintf(stderr, "cannot open GPIO backend\n");
        return EXIT_FAILURE;
    }
    struct swd_probe probe = {
        .io = {
            .ctx = &gpio,
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
    uint32_t idcode = 0U;
    bool ready = swd_switch_to_swd(&probe) &&
                 swd_read_idcode(&probe, &idcode) &&
                 swd_prepare_memory_access(&probe) &&
                 swd_transfer(&probe,
                              (struct swd_request){
                                  .ap = true, .read = false, .address = 4U},
                              address,
                              NULL) == SWD_ACK_OK;
    if (!ready) {
        fprintf(stderr, "CPU SWD setup failed\n");
        gpio_mmio_close(&gpio);
        return EXIT_FAILURE;
    }
    gpio_mmio_set_swdio_output(&gpio, true);
    gpio_mmio_write_swdio(&gpio, true);

    int gpio_fd = -1;
    volatile uint32_t *gpio_regs = map_registers(PERIPHERAL_BASE +
                                                     UINT32_C(0x00200000),
                                                 page_size,
                                                 &gpio_fd);
    int pwm_fd = -1;
    volatile uint32_t *pwm = map_registers(PWM_BASE, page_size, &pwm_fd);
    volatile uint32_t *sample_pwm = pwm == NULL
                                         ? NULL
                                         : pwm + (UINT32_C(0x800) / sizeof(uint32_t));
    int clock_fd = -1;
    volatile uint32_t *clock = map_registers(CLOCK_MANAGER_BASE,
                                             page_size,
                                             &clock_fd);
    int dma_fd = -1;
    volatile uint32_t *dma_registers = map_registers(PERIPHERAL_BASE + dma_base,
                                                     page_size,
                                                     &dma_fd);
    if (gpio_regs == NULL || pwm == NULL || sample_pwm == NULL ||
        clock == NULL || dma_registers == NULL) {
        fprintf(stderr, "cannot map DMA/PWM/GPIO registers\n");
        if (dma_registers != NULL) {
            unmap_registers(dma_registers, page_size, dma_fd);
        }
        if (clock != NULL) {
            unmap_registers(clock, page_size, clock_fd);
        }
        if (pwm != NULL) {
            unmap_registers(pwm, page_size, pwm_fd);
        }
        if (gpio_regs != NULL) {
            unmap_registers(gpio_regs, page_size, gpio_fd);
        }
        gpio_mmio_close(&gpio);
        return EXIT_FAILURE;
    }

    uint32_t original_fsel = gpio_regs[gpio_fsel1];
    uint32_t fsel_base = original_fsel & ~((7U << 0U) | (7U << 3U));
    struct ring_program ring = {0};
    if (!build_ring(&ring, fsel_base, dp_idcode)) {
        fprintf(stderr, "cannot build AP-read DMA ring (%zu CBs)\n", ring.count);
        unmap_registers(dma_registers, page_size, dma_fd);
        unmap_registers(clock, page_size, clock_fd);
        unmap_registers(pwm, page_size, pwm_fd);
        unmap_registers(gpio_regs, page_size, gpio_fd);
        gpio_mmio_close(&gpio);
        return EXIT_FAILURE;
    }

    size_t transactions = words + 1U;
    size_t sample_words = transactions * ring.dreq_count + ring.dreq_count;
    size_t sample_cb_count =
        (sample_words + max_sample_words_per_cb - 1U) /
        max_sample_words_per_cb;
    size_t output_cb_offset = 0U;
    size_t sample_cb_offset = align32(output_cb_offset +
                                      ring.count * sizeof(struct dma_cb));
    size_t output_data_offset = align32(sample_cb_offset +
                                        sample_cb_count * sizeof(struct dma_cb));
    size_t sample_data_offset = align32(output_data_offset +
                                        ring.count * sizeof(uint32_t));
    size_t allocation_size = sample_data_offset + sample_words * sizeof(uint32_t);
    struct vcsm_block block = {0};
    if (!vcsm_block_alloc(allocation_size, &block)) {
        fprintf(stderr, "cannot allocate %zu bytes of DMA memory\n", allocation_size);
        unmap_registers(dma_registers, page_size, dma_fd);
        unmap_registers(clock, page_size, clock_fd);
        unmap_registers(pwm, page_size, pwm_fd);
        unmap_registers(gpio_regs, page_size, gpio_fd);
        gpio_mmio_close(&gpio);
        return EXIT_FAILURE;
    }

    struct dma_cb *output_cbs = (struct dma_cb *)((uint8_t *)block.mapping +
                                                   output_cb_offset);
    struct dma_cb *sample_cbs = (struct dma_cb *)((uint8_t *)block.mapping +
                                                   sample_cb_offset);
    uint32_t *output_data = (uint32_t *)((uint8_t *)block.mapping +
                                         output_data_offset);
    uint32_t *sample_data = (uint32_t *)((uint8_t *)block.mapping +
                                         sample_data_offset);
    for (size_t index = 0U; index < ring.count; ++index) {
        output_data[index] = ring.values[index];
        size_t next_index = index + 1U < ring.count ? index + 1U : 2U;
        uint32_t next = block.bus_address +
                        (uint32_t)(next_index * sizeof(struct dma_cb));
        uint32_t output_info = dma_cb_no_wide_bursts | dma_cb_wait_resp;
        if (ring.paced[index]) {
            output_info |= dma_cb_dest_dreq | DMA_TI_PERMAP(5U);
        } else {
            output_info |= dma_cb_src_inc;
        }
        initialize_cb(&output_cbs[index],
                      block.bus_address +
                          (uint32_t)(output_data_offset +
                                     index * sizeof(uint32_t)),
                      ring.destinations[index],
                      output_info,
                      sizeof(uint32_t),
                      next);
    }
    size_t remaining = sample_words;
    size_t sample_offset_words = 0U;
    uint32_t sample_info = dma_cb_no_wide_bursts |
                           dma_cb_wait_resp |
                           dma_cb_dest_inc |
                           (sample_destination_dreq
                                ? dma_cb_dest_dreq
                                : dma_cb_src_dreq) |
                           DMA_TI_PERMAP(1U);
    for (size_t index = 0U; index < sample_cb_count; ++index) {
        size_t count = remaining > max_sample_words_per_cb
                           ? max_sample_words_per_cb
                           : remaining;
        uint32_t next = index + 1U < sample_cb_count
                            ? block.bus_address +
                                  (uint32_t)((sample_cb_offset +
                                              (index + 1U) * sizeof(struct dma_cb)))
                            : 0U;
        initialize_cb(&sample_cbs[index],
                      GPIO_BUS_BASE + UINT32_C(0x34),
                      block.bus_address +
                          (uint32_t)(sample_data_offset +
                                     sample_offset_words * sizeof(uint32_t)),
                      sample_info,
                      (uint32_t)(count * sizeof(uint32_t)),
                      next);
        sample_offset_words += count;
        remaining -= count;
    }
    volatile uint32_t *output_dma = dma_registers +
                                    output_dma_channel * 0x100U / sizeof(uint32_t);
    volatile uint32_t *sample_dma = dma_registers +
                                    sample_dma_channel * 0x100U / sizeof(uint32_t);
    reset_dma_channel(output_dma);
    reset_dma_channel(sample_dma);
    unsigned long actual_dreq_hz = 0UL;
    if (!configure_pacer(clock,
                         pwm,
                         sample_pwm,
                         dreq_hz,
                         &actual_dreq_hz)) {
        fprintf(stderr, "cannot configure PWM DREQ pacer\n");
        vcsm_block_free(&block);
        unmap_registers(dma_registers, page_size, dma_fd);
        unmap_registers(clock, page_size, clock_fd);
        unmap_registers(pwm, page_size, pwm_fd);
        unmap_registers(gpio_regs, page_size, gpio_fd);
        gpio_mmio_close(&gpio);
        return EXIT_FAILURE;
    }
    output_dma[1] = block.bus_address + (uint32_t)output_cb_offset;
    sample_dma[1] = block.bus_address + (uint32_t)sample_cb_offset;
    __sync_synchronize();
    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    uint32_t active_flags = DMA_CS_PRIORITY(7U) |
                            DMA_CS_PANIC_PRIORITY(7U) |
                            DMA_CS_DISDEBUG |
                            dma_cs_active;
    sample_dma[0] = active_flags;
    output_dma[0] = active_flags;

    bool complete = false;
    for (unsigned attempt = 0U; attempt < 100000000U; ++attempt) {
        if ((sample_dma[0] & dma_cs_active) == 0U) {
            complete = true;
            break;
        }
        if ((attempt & 0xffffU) == 0U) {
            struct timespec now = {0};
            (void)clock_gettime(CLOCK_MONOTONIC_RAW, &now);
            if (elapsed_seconds(&start, &now) > 10.0) {
                break;
            }
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    uint32_t sample_status = sample_dma[0];
    reset_dma_channel(output_dma);
    reset_dma_channel(sample_dma);
    stop_pacer(clock, pwm, sample_pwm);
    gpio_regs[gpio_fsel1] = original_fsel;
    __sync_synchronize();
    unmap_registers(dma_registers, page_size, dma_fd);
    unmap_registers(clock, page_size, clock_fd);
    unmap_registers(pwm, page_size, pwm_fd);
    unmap_registers(gpio_regs, page_size, gpio_fd);

    uint32_t *words_buffer = calloc(words, sizeof(uint32_t));
    size_t phase = 0U;
    bool set_samples = false;
    bool valid = complete &&
                 parse_stream(sample_data,
                              sample_words,
                              &ring,
                              transactions,
                              words_buffer,
                              &phase,
                              &set_samples);
    if (!valid) {
        fprintf(stderr,
                "stream DMA failed complete=%u status=0x%08" PRIx32
                " ring=%zu samples=%zu\n",
                complete ? 1U : 0U,
                sample_status,
                ring.count,
                sample_words);
        fprintf(stderr, "first-sample-bits=");
        size_t preview = sample_words < ring.dreq_count * 2U
                              ? sample_words
                              : ring.dreq_count * 2U;
        for (size_t index = 0U; index < preview; ++index) {
            fputc(sample_gpio_bit(sample_data[index]) != 0U ? '1' : '0', stderr);
        }
        fputc('\n', stderr);
        for (size_t candidate_phase = 0U;
             candidate_phase < ring.dreq_count;
             ++candidate_phase) {
            for (unsigned use_set = 0U; use_set < 2U; ++use_set) {
                uint32_t ack = 0U;
                unsigned ones = 0U;
                uint32_t value = 0U;
                for (unsigned bit = 0U; bit < 3U; ++bit) {
                    size_t index = candidate_phase +
                                   (use_set != 0U
                                        ? ring.set_sample_index[bit]
                                        : ring.clear_sample_index[bit]);
                    ack |= sample_gpio_bit(sample_data[index]) << bit;
                }
                for (unsigned bit = 0U; bit < 32U; ++bit) {
                    size_t index = candidate_phase +
                                   (use_set != 0U
                                        ? ring.set_sample_index[3U + bit]
                                        : ring.clear_sample_index[3U + bit]);
                    uint32_t high = sample_gpio_bit(sample_data[index]);
                    value |= high << bit;
                    ones += high;
                }
                size_t parity_index = candidate_phase +
                                      (use_set != 0U
                                           ? ring.set_sample_index[35]
                                           : ring.clear_sample_index[35]);
                bool parity = sample_gpio_bit(sample_data[parity_index]) != 0U;
                if (ack == 1U || phase < 3U) {
                    fprintf(stderr,
                            "candidate phase=%zu sample=%s ack=%u value=0x%08" PRIx32
                            " parity=%u expected=%u\n",
                            candidate_phase,
                            use_set != 0U ? "set" : "clear",
                            ack,
                            value,
                            parity ? 1U : 0U,
                            ones & 1U);
                }
            }
        }
    }
    if (valid && output_path != NULL) {
        FILE *output = fopen(output_path, "wb");
        if (output == NULL ||
            fwrite(words_buffer, sizeof(uint32_t), words, output) != words) {
            fprintf(stderr, "cannot write output file '%s'\n", output_path);
            valid = false;
        }
        if (output != NULL) {
            (void)fclose(output);
        }
    }
    double seconds = elapsed_seconds(&start, &end);
    if (valid) {
        printf("DMA stream IDCODE: 0x%08" PRIx32 "\n", idcode);
        printf("DMA stream read: %zu bytes in %.6f s (%.1f KiB/s, "
               "dreq=%lu Hz, ring=%zu CBs, phase=%zu, sample=%s)\n",
               words * sizeof(uint32_t),
               seconds,
               ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
               actual_dreq_hz,
               ring.count,
               phase,
               set_samples ? "set" : "clear");
    }
    free(words_buffer);
    vcsm_block_free(&block);
    gpio_mmio_close(&gpio);
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
