#define _GNU_SOURCE

/*
 * Parallel, hardware-paced SWD read experiment.
 *
 * Five DMA channels consume one word per PWM0 DREQ:
 *   - GPCLR0 waveform values
 *   - GPSET0 waveform values
 *   - GPIO10 direction values in GPFSEL1
 *   - GPIO level samples
 *   - a dummy stream feeding PWM0's FIFO
 *
 * The waveform arrays are flat streams rather than one control block per GPIO
 * edge.  This is intended to test whether the Pi can remove CPU work from the
 * SWD bit loop without relying on two independently-running PWM FIFOs.
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
    gpio_set0 = 0x1c,
    gpio_clr0 = 0x28,
    gpio_lev0 = 0x34,
    pwm_ctl = 0x00 / 4,
    pwm_dmac = 0x08 / 4,
    pwm_rng1 = 0x10 / 4,
    pwm_fif1 = 0x18 / 4,
    clock_pwmctl = 0xa0 / 4,
    clock_pwmdiv = 0xa4 / 4,
    dma_base = 0x007000,
    clear_dma_channel = 2,
    set_dma_channel = 3,
    fsel_dma_channel = 4,
    pacer_dma_channel = 5,
    sample_dma_channel = 6,
    dma_cs_active = 1U << 0,
    dma_cb_wait_resp = 1U << 3,
    dma_cb_dest_inc = 1U << 4,
    dma_cb_dest_dreq = 1U << 6,
    dma_cb_src_inc = 1U << 8,
    dma_cb_src_dreq = 1U << 10,
    dma_cb_no_wide_bursts = 1U << 26,
    dma_cs_reset = 1U << 31,
    dma_cs_abort = 1U << 30,
    dma_cs_disdebug = 1U << 28,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define GPIO_BUS_BASE UINT32_C(0x7e200000)
#define PWM_BUS_BASE UINT32_C(0x7e20c000)
#define CLOCK_MANAGER_BASE (PERIPHERAL_BASE + UINT32_C(0x00101000))
#define PWM_BASE (PERIPHERAL_BASE + UINT32_C(0x0020c000))
#define DMA_CS_PRIORITY(value) ((uint32_t)(value) << 16U)
#define DMA_CS_PANIC_PRIORITY(value) ((uint32_t)(value) << 20U)
#define DMA_TI_PERMAP(value) ((uint32_t)(value) << 16U)
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
#define PLLD_PER_HZ 750000000UL
#define SWCLK_MASK (UINT32_C(1) << 11U)
#define SWDIO_MASK (UINT32_C(1) << 10U)
#define GPIO_FSEL_OUTPUT (UINT32_C(1) << 0U)
#define GPIO_FSEL_INPUT UINT32_C(0)

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

struct waveform {
    uint32_t *clear_values;
    uint32_t *set_values;
    uint32_t *fsel_values;
    size_t *sample_positions;
    size_t event_count;
    size_t event_capacity;
    size_t sample_count;
    size_t sample_capacity;
    bool swdio_high;
    bool swclk_high;
};

struct waveform_dma_memory {
    struct vcsm_block block;
    size_t cb_offset;
    size_t clear_offset;
    size_t set_offset;
    size_t fsel_offset;
    size_t pacer_offset;
    size_t samples_offset;
    struct dma_cb *cbs;
    uint32_t *clear_values;
    uint32_t *set_values;
    uint32_t *fsel_values;
    uint32_t *pacer_values;
    uint32_t *samples;
};

static size_t align32(size_t value)
{
    return (value + 31U) & ~(size_t)31U;
}

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
    memcpy(allocation.name, "rpi-fast-parallel", sizeof("rpi-fast-parallel"));
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

static bool waveform_reserve(struct waveform *waveform,
                             size_t event_capacity,
                             size_t sample_capacity)
{
    waveform->clear_values = calloc(event_capacity, sizeof(uint32_t));
    waveform->set_values = calloc(event_capacity, sizeof(uint32_t));
    waveform->fsel_values = calloc(event_capacity, sizeof(uint32_t));
    waveform->sample_positions = calloc(sample_capacity, sizeof(size_t));
    if (waveform->clear_values == NULL || waveform->set_values == NULL ||
        waveform->fsel_values == NULL || waveform->sample_positions == NULL) {
        free(waveform->clear_values);
        free(waveform->set_values);
        free(waveform->fsel_values);
        free(waveform->sample_positions);
        memset(waveform, 0, sizeof(*waveform));
        return false;
    }
    waveform->event_capacity = event_capacity;
    waveform->sample_capacity = sample_capacity;
    return true;
}

static void waveform_free(struct waveform *waveform)
{
    free(waveform->clear_values);
    free(waveform->set_values);
    free(waveform->fsel_values);
    free(waveform->sample_positions);
    memset(waveform, 0, sizeof(*waveform));
}

static bool append_event(struct waveform *waveform,
                         uint32_t clear_value,
                         uint32_t set_value,
                         uint32_t fsel_value,
                         bool sample)
{
    if (waveform->event_count >= waveform->event_capacity ||
        (sample && waveform->sample_count >= waveform->sample_capacity)) {
        return false;
    }
    size_t index = waveform->event_count++;
    waveform->clear_values[index] = clear_value;
    waveform->set_values[index] = set_value;
    waveform->fsel_values[index] = fsel_value;
    if (sample) {
        waveform->sample_positions[waveform->sample_count++] = index;
    }
    return true;
}

static bool append_output_setup(struct waveform *waveform,
                                uint32_t fsel_output)
{
    return append_event(waveform, 0U, SWDIO_MASK, fsel_output, false);
}

static bool append_output_cycle(struct waveform *waveform,
                                bool bit,
                                uint32_t fsel_output)
{
    uint32_t clear_value = SWCLK_MASK;
    uint32_t set_value = 0U;
    if (bit != waveform->swdio_high) {
        if (bit) {
            set_value = SWDIO_MASK;
        } else {
            clear_value |= SWDIO_MASK;
        }
        waveform->swdio_high = bit;
    }
    if (!append_event(waveform, clear_value, set_value, fsel_output, false) ||
        !append_event(waveform, 0U, SWCLK_MASK, fsel_output, false)) {
        return false;
    }
    waveform->swclk_high = true;
    return true;
}

static bool append_input_cycle(struct waveform *waveform,
                               bool sample,
                               uint32_t fsel_input)
{
    if (!append_event(waveform, SWCLK_MASK, 0U, fsel_input, sample) ||
        !append_event(waveform, 0U, SWCLK_MASK, fsel_input, false)) {
        return false;
    }
    waveform->swclk_high = true;
    return true;
}

static bool build_waveform(struct waveform *waveform,
                           size_t transactions,
                           bool dp_idcode,
                           uint32_t fsel_base)
{
    uint32_t fsel_input = fsel_base | GPIO_FSEL_INPUT | GPIO_FSEL_OUTPUT;
    uint32_t fsel_output = fsel_base | GPIO_FSEL_OUTPUT | GPIO_FSEL_OUTPUT << 3U;
    /* GPIO11 is kept as output; GPIO10 is the only direction-switched pin. */
    fsel_input = fsel_base | (GPIO_FSEL_OUTPUT << 3U);
    fsel_output = fsel_base | GPIO_FSEL_OUTPUT | (GPIO_FSEL_OUTPUT << 3U);

    waveform->swdio_high = true;
    waveform->swclk_high = true;
    if (!append_output_setup(waveform, fsel_output)) {
        return false;
    }

    uint8_t request = swd_encode_request(
        (struct swd_request){
            .ap = !dp_idcode,
            .read = true,
            .address = dp_idcode ? 0U : 12U,
        });
    for (size_t transaction = 0U; transaction < transactions; ++transaction) {
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            if (!append_output_cycle(waveform,
                                     ((request >> bit) & 1U) != 0U,
                                     fsel_output)) {
                return false;
            }
        }
        /* SWD turnaround: release GPIO10, then give the target one cycle. */
        if (!append_input_cycle(waveform, false, fsel_input)) {
            return false;
        }
        for (unsigned bit = 0U; bit < 36U; ++bit) {
            if (!append_input_cycle(waveform, true, fsel_input)) {
                return false;
            }
        }
        /* SWD turnaround back to host output, followed by an idle setup event. */
        if (!append_input_cycle(waveform, false, fsel_input) ||
            !append_output_setup(waveform, fsel_output)) {
            return false;
        }
    }
    return true;
}

static void initialize_cb(struct dma_cb *cb,
                          uint32_t source,
                          uint32_t destination,
                          uint32_t info,
                          uint32_t transfer_length)
{
    cb->info = info;
    cb->source = source;
    cb->destination = destination;
    cb->transfer_length = transfer_length;
    cb->stride = 0U;
    cb->next = 0U;
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

static void reset_dma_channel(volatile uint32_t *channel)
{
    channel[0] = dma_cs_abort;
    usleep(10U);
    channel[0] = dma_cs_reset;
    usleep(10U);
}

static uint32_t dma_run_flags(unsigned priority)
{
    return DMA_CS_PRIORITY(priority) |
           DMA_CS_PANIC_PRIORITY(priority) |
           dma_cs_disdebug |
           dma_cs_active;
}

static bool all_dma_idle(volatile uint32_t *dma_registers)
{
    const unsigned channels[] = {
        clear_dma_channel,
        set_dma_channel,
        fsel_dma_channel,
        pacer_dma_channel,
        sample_dma_channel,
    };
    for (size_t index = 0U; index < sizeof(channels) / sizeof(channels[0]); ++index) {
        volatile uint32_t *channel = dma_registers +
                                      channels[index] * 0x100U / sizeof(uint32_t);
        if ((channel[0] & dma_cs_active) != 0U) {
            return false;
        }
    }
    return true;
}

static bool decode_sample(const uint32_t *samples,
                          size_t sample_count,
                          size_t position,
                          int delta,
                          uint32_t *value)
{
    long signed_index = (long)position + (long)delta;
    if (signed_index < 0L || (size_t)signed_index >= sample_count) {
        return false;
    }
    uint32_t bits[36] = {0U};
    size_t base = (size_t)signed_index;
    for (unsigned bit = 0U; bit < 36U; ++bit) {
        bits[bit] = (samples[base + bit * 2U] & SWDIO_MASK) != 0U ? 1U : 0U;
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

static bool decode_waveform(const struct waveform *waveform,
                            const uint32_t *samples,
                            size_t sample_count,
                            size_t transactions,
                            size_t words,
                            bool dp_idcode,
                            uint32_t *output,
                            int *delta_out,
                            uint32_t *idcode_out)
{
    for (int delta = -8; delta <= 8; ++delta) {
        bool valid = true;
        uint32_t observed_idcode = 0U;
        for (size_t transaction = 0U; transaction < transactions; ++transaction) {
            uint32_t value = 0U;
            size_t position = waveform->sample_positions[transaction * 36U];
            if (!decode_sample(samples,
                               sample_count,
                               position,
                               delta,
                               &value)) {
                valid = false;
                break;
            }
            if (transaction == 0U) {
                observed_idcode = value;
            } else if (!dp_idcode && transaction - 1U < words) {
                output[transaction - 1U] = value;
            }
        }
        if (valid && (dp_idcode ? observed_idcode == UINT32_C(0x6ba02477) : true)) {
            *delta_out = delta;
            *idcode_out = observed_idcode;
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
    const char *output_path = "/tmp/dma-parallel-read.bin";
    bool valid = false;
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
        } else if (strcmp(argv[index], "--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else {
            fprintf(stderr,
                    "Usage: %s [--address HEX] [--words COUNT] [--dreq HZ] "
                    "[--dp] [--output FILE]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }

    size_t transactions = dp_idcode ? 1U : words + 1U;
    size_t event_capacity = 1U + transactions * 94U;
    size_t sample_capacity = transactions * 36U;
    struct waveform waveform = {0};
    if (!waveform_reserve(&waveform, event_capacity, sample_capacity)) {
        fprintf(stderr, "cannot allocate waveform arrays\n");
        return EXIT_FAILURE;
    }

    struct gpio_mmio gpio = {0};
    if (gpio_mmio_open(&gpio, 11U, 10U, 6000000U) != 0) {
        fprintf(stderr, "cannot open GPIO backend\n");
        waveform_free(&waveform);
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
                 swd_read_idcode(&probe, &idcode);
    if (!dp_idcode) {
        ready = ready && swd_prepare_memory_access(&probe) &&
                swd_transfer(&probe,
                             (struct swd_request){
                                 .ap = true, .read = false, .address = 4U},
                             address,
                             NULL) == SWD_ACK_OK;
    }
    if (!ready) {
        fprintf(stderr, "CPU SWD setup failed\n");
        gpio_mmio_close(&gpio);
        waveform_free(&waveform);
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
    int clock_fd = -1;
    volatile uint32_t *clock = map_registers(CLOCK_MANAGER_BASE,
                                             page_size,
                                             &clock_fd);
    int dma_fd = -1;
    volatile uint32_t *dma_registers = map_registers(PERIPHERAL_BASE + dma_base,
                                                     page_size,
                                                     &dma_fd);
    if (gpio_regs == NULL || pwm == NULL || clock == NULL || dma_registers == NULL) {
        fprintf(stderr, "cannot map GPIO/PWM/clock/DMA registers\n");
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
        waveform_free(&waveform);
        return EXIT_FAILURE;
    }

    uint32_t original_fsel = gpio_regs[gpio_fsel1];
    uint32_t fsel_base = original_fsel & ~((7U << 0U) | (7U << 3U));
    if (!build_waveform(&waveform,
                        transactions,
                        dp_idcode,
                        fsel_base)) {
        fprintf(stderr, "cannot build waveform: %zu events\n", waveform.event_count);
        goto cleanup_maps;
    }

    size_t cb_offset = 0U;
    size_t data_offset = align32(cb_offset + 5U * sizeof(struct dma_cb));
    size_t clear_offset = data_offset;
    size_t set_offset = align32(clear_offset + waveform.event_count * sizeof(uint32_t));
    size_t fsel_offset = align32(set_offset + waveform.event_count * sizeof(uint32_t));
    size_t pacer_offset = align32(fsel_offset + waveform.event_count * sizeof(uint32_t));
    size_t samples_offset = align32(pacer_offset + waveform.event_count * sizeof(uint32_t));
    size_t allocation_size = samples_offset + waveform.event_count * sizeof(uint32_t);
    struct waveform_dma_memory memory = {0};
    if (!vcsm_block_alloc(allocation_size, &memory.block)) {
        fprintf(stderr, "cannot allocate %zu bytes of DMA memory\n", allocation_size);
        goto cleanup_maps;
    }
    memory.cb_offset = cb_offset;
    memory.clear_offset = clear_offset;
    memory.set_offset = set_offset;
    memory.fsel_offset = fsel_offset;
    memory.pacer_offset = pacer_offset;
    memory.samples_offset = samples_offset;
    memory.cbs = (struct dma_cb *)((uint8_t *)memory.block.mapping + cb_offset);
    memory.clear_values = (uint32_t *)((uint8_t *)memory.block.mapping + clear_offset);
    memory.set_values = (uint32_t *)((uint8_t *)memory.block.mapping + set_offset);
    memory.fsel_values = (uint32_t *)((uint8_t *)memory.block.mapping + fsel_offset);
    memory.pacer_values = (uint32_t *)((uint8_t *)memory.block.mapping + pacer_offset);
    memory.samples = (uint32_t *)((uint8_t *)memory.block.mapping + samples_offset);
    memcpy(memory.clear_values,
           waveform.clear_values,
           waveform.event_count * sizeof(uint32_t));
    memcpy(memory.set_values,
           waveform.set_values,
           waveform.event_count * sizeof(uint32_t));
    memcpy(memory.fsel_values,
           waveform.fsel_values,
           waveform.event_count * sizeof(uint32_t));
    memset(memory.pacer_values, 0, waveform.event_count * sizeof(uint32_t));

    uint32_t output_info = dma_cb_no_wide_bursts |
                           dma_cb_wait_resp |
                           dma_cb_src_inc |
                           dma_cb_dest_dreq |
                           DMA_TI_PERMAP(5U);
    uint32_t sample_info = dma_cb_no_wide_bursts |
                           dma_cb_wait_resp |
                           dma_cb_dest_inc |
                           dma_cb_src_dreq |
                           DMA_TI_PERMAP(5U);
    uint32_t pacer_info = dma_cb_no_wide_bursts |
                          dma_cb_wait_resp |
                          dma_cb_src_inc |
                          dma_cb_dest_dreq |
                          DMA_TI_PERMAP(5U);
    initialize_cb(&memory.cbs[0],
                  memory.block.bus_address + (uint32_t)clear_offset,
                  GPIO_BUS_BASE + gpio_clr0,
                  output_info,
                  (uint32_t)(waveform.event_count * sizeof(uint32_t)));
    initialize_cb(&memory.cbs[1],
                  memory.block.bus_address + (uint32_t)set_offset,
                  GPIO_BUS_BASE + gpio_set0,
                  output_info,
                  (uint32_t)(waveform.event_count * sizeof(uint32_t)));
    initialize_cb(&memory.cbs[2],
                  memory.block.bus_address + (uint32_t)fsel_offset,
                  GPIO_BUS_BASE + UINT32_C(0x04),
                  output_info,
                  (uint32_t)(waveform.event_count * sizeof(uint32_t)));
    initialize_cb(&memory.cbs[3],
                  memory.block.bus_address + (uint32_t)pacer_offset,
                  PWM_BUS_BASE + UINT32_C(0x18),
                  pacer_info,
                  (uint32_t)(waveform.event_count * sizeof(uint32_t)));
    initialize_cb(&memory.cbs[4],
                  GPIO_BUS_BASE + gpio_lev0,
                  memory.block.bus_address + (uint32_t)samples_offset,
                  sample_info,
                  (uint32_t)(waveform.event_count * sizeof(uint32_t)));

    volatile uint32_t *clear_dma = dma_registers +
                                    clear_dma_channel * 0x100U / sizeof(uint32_t);
    volatile uint32_t *set_dma = dma_registers +
                                  set_dma_channel * 0x100U / sizeof(uint32_t);
    volatile uint32_t *fsel_dma = dma_registers +
                                   fsel_dma_channel * 0x100U / sizeof(uint32_t);
    volatile uint32_t *pacer_dma = dma_registers +
                                    pacer_dma_channel * 0x100U / sizeof(uint32_t);
    volatile uint32_t *sample_dma = dma_registers +
                                     sample_dma_channel * 0x100U / sizeof(uint32_t);
    reset_dma_channel(clear_dma);
    reset_dma_channel(set_dma);
    reset_dma_channel(fsel_dma);
    reset_dma_channel(pacer_dma);
    reset_dma_channel(sample_dma);

    unsigned long actual_dreq_hz = 0UL;
    if (!configure_pacer(clock, pwm, dreq_hz, &actual_dreq_hz)) {
        fprintf(stderr, "cannot configure PWM DREQ pacer\n");
        vcsm_block_free(&memory.block);
        goto cleanup_maps;
    }
    pwm[pwm_fif1] = 0U;
    uint32_t cb_base = memory.block.bus_address + (uint32_t)cb_offset;
    clear_dma[1] = cb_base + 0U * sizeof(struct dma_cb);
    set_dma[1] = cb_base + 1U * sizeof(struct dma_cb);
    fsel_dma[1] = cb_base + 2U * sizeof(struct dma_cb);
    pacer_dma[1] = cb_base + 3U * sizeof(struct dma_cb);
    sample_dma[1] = cb_base + 4U * sizeof(struct dma_cb);
    __sync_synchronize();

    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    clear_dma[0] = dma_run_flags(7U);
    set_dma[0] = dma_run_flags(7U);
    fsel_dma[0] = dma_run_flags(7U);
    sample_dma[0] = dma_run_flags(5U);
    pacer_dma[0] = dma_run_flags(7U);
    bool complete = false;
    for (unsigned attempt = 0U; attempt < 100000000U; ++attempt) {
        if (all_dma_idle(dma_registers)) {
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
    uint32_t clear_status = clear_dma[0];
    uint32_t set_status = set_dma[0];
    uint32_t fsel_status = fsel_dma[0];
    uint32_t pacer_status = pacer_dma[0];
    uint32_t sample_status = sample_dma[0];
    reset_dma_channel(clear_dma);
    reset_dma_channel(set_dma);
    reset_dma_channel(fsel_dma);
    reset_dma_channel(pacer_dma);
    reset_dma_channel(sample_dma);
    stop_pacer(clock, pwm);
    gpio_regs[gpio_fsel1] = original_fsel;
    __sync_synchronize();

    uint32_t *words_buffer = calloc(words, sizeof(uint32_t));
    int delta = 0;
    uint32_t observed_idcode = 0U;
    valid = complete && words_buffer != NULL &&
                 decode_waveform(&waveform,
                                 memory.samples,
                                 waveform.event_count,
                                 transactions,
                                 words,
                                 dp_idcode,
                                 words_buffer,
                                 &delta,
                                 &observed_idcode);
    if (valid && !dp_idcode) {
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
    if (!valid) {
        fprintf(stderr,
                "parallel DMA failed complete=%u statuses=%08" PRIx32 "/%08" PRIx32
                "/%08" PRIx32 "/%08" PRIx32 "/%08" PRIx32
                " events=%zu samples=%zu\n",
                complete ? 1U : 0U,
                clear_status,
                set_status,
                fsel_status,
                pacer_status,
                sample_status,
                waveform.event_count,
                waveform.sample_count);
        fprintf(stderr, "sample-bits=");
        size_t preview = waveform.event_count < 128U ? waveform.event_count : 128U;
        for (size_t index = 0U; index < preview; ++index) {
            fputc((memory.samples[index] & SWDIO_MASK) != 0U ? '1' : '0', stderr);
        }
        fputc('\n', stderr);
        for (int candidate_delta = -8; candidate_delta <= 8; ++candidate_delta) {
            long signed_index = (long)waveform.sample_positions[0] +
                                (long)candidate_delta;
            if (signed_index < 0L ||
                (size_t)signed_index + 70U >= waveform.event_count) {
                continue;
            }
            size_t base = (size_t)signed_index;
            unsigned ack = 0U;
            uint32_t preview_value = 0U;
            for (unsigned bit = 0U; bit < 3U; ++bit) {
                ack |= ((memory.samples[base + bit * 2U] & SWDIO_MASK) != 0U
                            ? 1U
                            : 0U)
                       << bit;
            }
            for (unsigned bit = 0U; bit < 32U; ++bit) {
                if ((memory.samples[base + (3U + bit) * 2U] & SWDIO_MASK) != 0U) {
                    preview_value |= UINT32_C(1) << bit;
                }
            }
            fprintf(stderr,
                    "candidate delta=%d ack=%u value=0x%08" PRIx32 "\n",
                    candidate_delta,
                    ack,
                    preview_value);
            if (candidate_delta >= -1 && candidate_delta <= 3) {
                fprintf(stderr, "  bits=");
                for (unsigned bit = 0U; bit < 36U; ++bit) {
                    fputc((memory.samples[base + bit * 2U] & SWDIO_MASK) != 0U
                              ? '1'
                              : '0',
                          stderr);
                }
                fputc('\n', stderr);
            }
        }
    } else if (dp_idcode) {
        printf("parallel DMA IDCODE: 0x%08" PRIx32 "\n", observed_idcode);
    } else {
        double seconds = elapsed_seconds(&start, &end);
        printf("parallel DMA read: %zu bytes in %.6f s (%.1f KiB/s, "
               "dreq=%lu Hz, events=%zu, sample-delta=%d)\n",
               words * sizeof(uint32_t),
               seconds,
               ((double)words * sizeof(uint32_t) / 1024.0) / seconds,
               actual_dreq_hz,
               waveform.event_count,
               delta);
    }
    free(words_buffer);
    vcsm_block_free(&memory.block);

cleanup_maps:
    unmap_registers(dma_registers, page_size, dma_fd);
    unmap_registers(clock, page_size, clock_fd);
    unmap_registers(pwm, page_size, pwm_fd);
    unmap_registers(gpio_regs, page_size, gpio_fd);
    gpio_mmio_close(&gpio);
    waveform_free(&waveform);
    return valid ? EXIT_SUCCESS : EXIT_FAILURE;
}
