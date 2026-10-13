#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
    gpio_fsel0 = 0x00 / 4,
    gpio_set0 = 0x1c / 4,
    gpio_clr0 = 0x28 / 4,
    pwm_ctl = 0x00 / 4,
    pwm_dmac = 0x08 / 4,
    pwm_rng1 = 0x10 / 4,
    pwm_fif1 = 0x18 / 4,
    clock_pwmctl = 0xa0 / 4,
    clock_pwmdiv = 0xa4 / 4,
    gpio_bus_base = 0x7e200000,
    dma_base = 0x007000,
    dma_channel = 5,
    dma_cb_src_inc = 1U << 8,
    dma_cb_no_wide_bursts = 1U << 26,
    dma_cb_tmode = 1U << 1,
    dma_cb_dest_dreq = 1U << 6,
    dma_cb_wait_resp = 1U << 3,
    dma_cs_end = 1U << 1,
    dma_cs_active = 1U << 0,
};

#define PERIPHERAL_BASE UINT32_C(0xfe000000)
#define CLOCK_MANAGER_BASE (PERIPHERAL_BASE + UINT32_C(0x00101000))
#define PWM_BASE (PERIPHERAL_BASE + UINT32_C(0x0020c000))
#define PWM_BUS_BASE UINT32_C(0x7e20c000)
#define DMA_CS_RESET UINT32_C(1) << 31
#define DMA_CS_ABORT UINT32_C(1) << 30
#define DMA_CS_DISDEBUG UINT32_C(1) << 28
#define CM_PASSWORD UINT32_C(0x5a000000)
#define CM_CTL_BUSY (UINT32_C(1) << 7)
#define CM_CTL_KILL (UINT32_C(1) << 5)
#define CM_CTL_ENABLE (UINT32_C(1) << 4)
#define CM_CTL_PLLD_PER UINT32_C(6)
#define PWM_CTL_CLEAR_FIFO (UINT32_C(1) << 6)
#define PWM_CTL_MODE1 (UINT32_C(1) << 1)
#define PWM_CTL_USEF1 (UINT32_C(1) << 5)
#define PWM_CTL_PWEN1 UINT32_C(1)
#define PWM_DMAC_ENABLE (UINT32_C(1) << 31)
#define PWM_DMAC_PANIC_THRESHOLD (UINT32_C(7) << 8)
#define PWM_DMAC_DREQ_THRESHOLD UINT32_C(1)
#define DMA_TI_PERMAP(value) ((uint32_t)(value) << 16)

/* BCM2711's PLLD_PER is nominally 750 MHz. The measured transfer duration is
 * still reported, so the result does not depend on this being exact. */
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

static void initialize_cb(struct dma_cb *cb,
                          uint32_t source,
                          uint32_t destination,
                          uint32_t info,
                          uint32_t transfer_length,
                          uint32_t stride,
                          uint32_t next)
{
    cb->info = info;
    cb->source = source;
    cb->destination = destination;
    cb->transfer_length = transfer_length;
    cb->stride = stride;
    cb->next = next;
    cb->reserved[0] = 0U;
    cb->reserved[1] = 0U;
}

struct vcsm_block {
    void *mapping;
    size_t size;
    uint32_t bus_address;
    int allocation_fd;
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

#define VC_SM_CMA_IOCTL_MEM_ALLOC _IOR('J', 0x5a, struct vcsm_alloc)

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
    memcpy(allocation.name, "rpi-fast-gpio", sizeof("rpi-fast-gpio"));
    if (ioctl(vcsm_fd, VC_SM_CMA_IOCTL_MEM_ALLOC, &allocation) < 0 || allocation.handle < 0) {
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
    if (mapping == MAP_FAILED || allocation.dma_address > UINT32_MAX) {
        if (mapping != MAP_FAILED) {
            (void)munmap(mapping, rounded);
        }
        (void)close(allocation.handle);
        return false;
    }
    block->mapping = mapping;
    block->size = rounded;
    block->bus_address = (uint32_t)allocation.dma_address;
    block->allocation_fd = allocation.handle;
    fprintf(stderr,
            "VCSM DMA memory bus=0x%08" PRIx32 " map=%p size=%zu\n",
            block->bus_address,
            mapping,
            rounded);
    memset(mapping, 0, rounded);
    return true;
}

static void vcsm_block_free(struct vcsm_block *block)
{
    if (block->mapping == NULL) {
        return;
    }
    (void)munmap(block->mapping, block->size);
    (void)close(block->allocation_fd);
    block->mapping = NULL;
}

static volatile uint32_t *map_registers(uint32_t physical_address, size_t size, int *fd_out)
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

static double elapsed_seconds(const struct timespec *start, const struct timespec *end)
{
    time_t seconds = end->tv_sec - start->tv_sec;
    long nanoseconds = end->tv_nsec - start->tv_nsec;
    return (double)seconds + (double)nanoseconds / 1000000000.0;
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

static bool configure_pwm_pacer(volatile uint32_t *clock,
                                volatile uint32_t *pwm,
                                unsigned long pulse_hz,
                                unsigned long *actual_pulse_hz)
{
    if (pulse_hz == 0UL || pulse_hz > PLLD_PER_HZ / 2UL) {
        return false;
    }

    /* Each PWM FIFO word supplies one DMA request in serialiser mode. The
     * waveform emits two GPIO writes per pulse, so request twice per pulse. */
    unsigned long requested_word_hz = pulse_hz * 2UL;
    unsigned long divisor = (PLLD_PER_HZ + requested_word_hz / 2UL) /
                            requested_word_hz;
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
    clock[clock_pwmdiv] = CM_PASSWORD | ((uint32_t)divisor << 12);
    clock[clock_pwmctl] = CM_PASSWORD | CM_CTL_PLLD_PER | CM_CTL_ENABLE;

    pwm[pwm_ctl] = 0U;
    pwm[pwm_dmac] = 0U;
    pwm[pwm_ctl] = PWM_CTL_CLEAR_FIFO;
    pwm[pwm_rng1] = 1U;
    pwm[pwm_dmac] = PWM_DMAC_ENABLE |
                    PWM_DMAC_PANIC_THRESHOLD |
                    PWM_DMAC_DREQ_THRESHOLD;
    pwm[pwm_ctl] = PWM_CTL_MODE1 | PWM_CTL_USEF1 | PWM_CTL_PWEN1;

    *actual_pulse_hz = PLLD_PER_HZ / divisor / 2UL;
    return true;
}

static void stop_pwm_pacer(volatile uint32_t *clock, volatile uint32_t *pwm)
{
    pwm[pwm_ctl] = 0U;
    pwm[pwm_dmac] = 0U;
    clock[clock_pwmctl] = CM_PASSWORD | CM_CTL_KILL;
    (void)wait_clock_idle(clock);
}

int main(int argc, char **argv)
{
    unsigned gpio_pin = 11U;
    unsigned long pulse_count = 1000000UL;
    unsigned long paced_hz = 0UL;
    for (int index = 1; index < argc; ++index) {
        unsigned long value = 0UL;
        if (strcmp(argv[index], "--gpio") == 0 && index + 1 < argc &&
            parse_unsigned(argv[++index], &value)) {
            gpio_pin = (unsigned)value;
        } else if (strcmp(argv[index], "--pulses") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value)) {
            pulse_count = value;
        } else if (strcmp(argv[index], "--paced-hz") == 0 && index + 1 < argc &&
                   parse_unsigned(argv[++index], &value)) {
            paced_hz = value;
        } else {
            fprintf(stderr,
                    "Usage: %s [--gpio PIN] [--pulses COUNT] [--paced-hz HZ]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (gpio_pin > 31U || pulse_count == 0UL || pulse_count > 10000000UL ||
        paced_hz > PLLD_PER_HZ / 2UL) {
        fprintf(stderr,
                "GPIO must be 0..31, pulses must be 1..10000000, and paced-hz must be <= %lu\n",
                PLLD_PER_HZ / 2UL);
        return EXIT_FAILURE;
    }

    size_t cb_count = paced_hz != 0UL
                          ? 1U + (size_t)pulse_count * 4U
                          : (size_t)pulse_count;
    size_t data_words = paced_hz != 0UL
                            ? cb_count
                            : (size_t)pulse_count * 2U;
    size_t cb_bytes = cb_count * sizeof(struct dma_cb);
    size_t data_offset = (cb_bytes + 31U) & ~(size_t)31U;
    size_t data_bytes = data_words * sizeof(uint32_t);
    struct vcsm_block block = {0};
    if (!vcsm_block_alloc(data_offset + data_bytes, &block)) {
        fprintf(stderr, "cannot allocate DMA-visible memory\n");
        return EXIT_FAILURE;
    }

    struct dma_cb *cbs = (struct dma_cb *)block.mapping;
    uint32_t *data = (uint32_t *)((uint8_t *)block.mapping + data_offset);
    uint32_t mask = 1U << gpio_pin;
    uint32_t gpio_set_bus = gpio_bus_base + gpio_set0 * sizeof(uint32_t);
    uint32_t gpio_clr_bus = gpio_bus_base + gpio_clr0 * sizeof(uint32_t);
    uint32_t pwm_fifo_bus = PWM_BUS_BASE + pwm_fif1 * sizeof(uint32_t);
    uint32_t paced_gpio_info = dma_cb_src_inc |
                               dma_cb_no_wide_bursts |
                               dma_cb_dest_dreq |
                               dma_cb_wait_resp |
                               DMA_TI_PERMAP(5U);
    uint32_t paced_fifo_info = dma_cb_src_inc |
                               dma_cb_no_wide_bursts |
                               dma_cb_wait_resp;
    if (paced_hz != 0UL) {
        data[0] = 0U;
        initialize_cb(&cbs[0],
                      block.bus_address + (uint32_t)data_offset,
                      pwm_fifo_bus,
                      paced_fifo_info,
                      sizeof(uint32_t),
                      0U,
                      block.bus_address + sizeof(struct dma_cb));
        for (unsigned long index = 0UL; index < pulse_count; ++index) {
            size_t cb_index = 1U + (size_t)index * 4U;
            size_t data_index = cb_index;
            uint32_t next_set = block.bus_address +
                                (uint32_t)((cb_index + 1U) * sizeof(struct dma_cb));
            uint32_t next_feed_after_set = block.bus_address +
                                           (uint32_t)((cb_index + 2U) * sizeof(struct dma_cb));
            uint32_t next_clear = block.bus_address +
                                  (uint32_t)((cb_index + 3U) * sizeof(struct dma_cb));
            uint32_t next_feed_after_clear = index + 1UL < pulse_count
                                                 ? block.bus_address +
                                                       (uint32_t)((cb_index + 4U) *
                                                                  sizeof(struct dma_cb))
                                                 : 0U;
            data[data_index] = mask;
            data[data_index + 1U] = 0U;
            data[data_index + 2U] = mask;
            data[data_index + 3U] = 0U;
            initialize_cb(&cbs[cb_index],
                          block.bus_address + (uint32_t)(data_offset +
                                                         data_index * sizeof(uint32_t)),
                          gpio_set_bus,
                          paced_gpio_info,
                          sizeof(uint32_t),
                          0U,
                          next_set);
            initialize_cb(&cbs[cb_index + 1U],
                          block.bus_address + (uint32_t)(data_offset +
                                                         (data_index + 1U) *
                                                             sizeof(uint32_t)),
                          pwm_fifo_bus,
                          paced_fifo_info,
                          sizeof(uint32_t),
                          0U,
                          next_feed_after_set);
            initialize_cb(&cbs[cb_index + 2U],
                          block.bus_address + (uint32_t)(data_offset +
                                                         (data_index + 2U) *
                                                             sizeof(uint32_t)),
                          gpio_clr_bus,
                          paced_gpio_info,
                          sizeof(uint32_t),
                          0U,
                          next_clear);
            initialize_cb(&cbs[cb_index + 3U],
                          block.bus_address + (uint32_t)(data_offset +
                                                         (data_index + 3U) *
                                                             sizeof(uint32_t)),
                          pwm_fifo_bus,
                          paced_fifo_info,
                          sizeof(uint32_t),
                          0U,
                          next_feed_after_clear);
        }
    } else {
        for (unsigned long index = 0UL; index < pulse_count; ++index) {
            data[index * 2U] = mask;
            data[index * 2U + 1U] = mask;
            initialize_cb(&cbs[index],
                          block.bus_address + (uint32_t)(data_offset +
                                                         index * 2U *
                                                             sizeof(uint32_t)),
                          gpio_set_bus,
                          dma_cb_src_inc | dma_cb_no_wide_bursts | dma_cb_tmode,
                          (1U << 16) | 4U,
                          8U << 16,
                          index + 1UL < pulse_count
                              ? block.bus_address +
                                    (uint32_t)((index + 1UL) * sizeof(struct dma_cb))
                              : 0U);
        }
    }

    int gpio_fd = -1;
    volatile uint32_t *gpio = map_registers(0xfe200000U, page_size, &gpio_fd);
    if (gpio == NULL) {
        perror("GPIO map");
        vcsm_block_free(&block);
        return EXIT_FAILURE;
    }
    unsigned fsel_index = gpio_pin / 10U;
    unsigned fsel_shift = (gpio_pin % 10U) * 3U;
    gpio[gpio_fsel0 + fsel_index] &= ~(7U << fsel_shift);
    gpio[gpio_fsel0 + fsel_index] |= 1U << fsel_shift;
    gpio[gpio_clr0] = mask;

    int dma_fd = -1;
    volatile uint32_t *dma_registers = map_registers(PERIPHERAL_BASE + dma_base,
                                                     page_size,
                                                     &dma_fd);
    if (dma_registers == NULL) {
        perror("DMA map");
        unmap_registers(gpio, page_size, gpio_fd);
        vcsm_block_free(&block);
        return EXIT_FAILURE;
    }
    volatile uint32_t *dma = dma_registers + (dma_channel * 0x100U / sizeof(uint32_t));

    int pwm_fd = -1;
    int clock_fd = -1;
    volatile uint32_t *pwm = NULL;
    volatile uint32_t *clock = NULL;
    unsigned long actual_paced_hz = 0UL;
    if (paced_hz != 0UL) {
        pwm = map_registers(PWM_BASE, page_size, &pwm_fd);
        clock = map_registers(CLOCK_MANAGER_BASE, page_size, &clock_fd);
        if (pwm == NULL || clock == NULL ||
            !configure_pwm_pacer(clock, pwm, paced_hz, &actual_paced_hz)) {
            fprintf(stderr, "cannot configure PWM DREQ pacer\n");
            if (clock != NULL) {
                unmap_registers(clock, page_size, clock_fd);
            }
            if (pwm != NULL) {
                unmap_registers(pwm, page_size, pwm_fd);
            }
            unmap_registers(dma_registers, page_size, dma_fd);
            unmap_registers(gpio, page_size, gpio_fd);
            vcsm_block_free(&block);
            return EXIT_FAILURE;
        }
        fprintf(stderr,
                "PWM DREQ pacing requested=%lu Hz actual_nominal=%lu Hz\n",
                paced_hz,
                actual_paced_hz);
    }

    dma[0] = DMA_CS_ABORT;
    usleep(100U);
    dma[0] = DMA_CS_RESET;
    dma[1] = block.bus_address;
    __sync_synchronize();

    struct timespec start = {0};
    struct timespec end = {0};
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &start);
    dma[0] = (7U << 16) | (7U << 20) | DMA_CS_DISDEBUG | dma_cs_active;
    bool complete = false;
    for (unsigned attempt = 0U; attempt < 10000U; ++attempt) {
        usleep(1000U);
        if ((dma[0] & dma_cs_active) == 0U) {
            complete = true;
            break;
        }
    }
    (void)clock_gettime(CLOCK_MONOTONIC_RAW, &end);
    uint32_t status = dma[0];
    if (!complete) {
        dma[0] = DMA_CS_ABORT;
        fprintf(stderr, "DMA timeout, status=0x%08" PRIx32 "\n", status);
    } else {
        double seconds = elapsed_seconds(&start, &end);
        printf("DMA GPIO%u: %lu pulses in %.6f s (%.3f MHz pulse rate), status=0x%08" PRIx32 "\n",
               gpio_pin,
               pulse_count,
               seconds,
               (double)pulse_count / seconds / 1000000.0,
               status);
    }

    dma[0] = DMA_CS_ABORT;
    usleep(100U);
    dma[0] = DMA_CS_RESET;
    if (paced_hz != 0UL) {
        stop_pwm_pacer(clock, pwm);
        unmap_registers(clock, page_size, clock_fd);
        unmap_registers(pwm, page_size, pwm_fd);
    }
    unmap_registers(dma_registers, page_size, dma_fd);
    gpio[gpio_fsel0 + fsel_index] &= ~(7U << fsel_shift);
    unmap_registers(gpio, page_size, gpio_fd);
    vcsm_block_free(&block);
    return complete ? EXIT_SUCCESS : EXIT_FAILURE;
}
