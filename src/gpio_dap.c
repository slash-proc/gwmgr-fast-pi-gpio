#define _POSIX_C_SOURCE 200809L
#include "gpio_dap.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static const uint32_t cortex_m_dhcsr = 0xe000edf0U;
static const uint32_t cortex_m_dcrsr = 0xe000edf4U;
static const uint32_t cortex_m_dcrdr = 0xe000edf8U;
static const uint32_t cortex_m_aircr = 0xe000ed0cU;
static const uint32_t cortex_m_demcr = 0xe000edfcU;
static const uint32_t cortex_m_debug_key = 0xa05f0000U;
static const uint32_t cortex_m_dcrsr_write = 1U << 16;
static const uint32_t cortex_m_dhcsr_reg_ready = 1U << 16;

static uint8_t dap_ack(enum swd_ack ack)
{
    return (uint8_t)ack;
}

static bool decode_request(uint8_t request,
                           struct swd_request *decoded)
{
    /* The minimal backend does not implement value-match or timestamp
     * transfers; the low nibble is the complete SWD request. */
    if ((request & 0xf0U) != 0U || decoded == NULL) {
        return false;
    }
    decoded->ap = (request & 0x01U) != 0U;
    decoded->read = (request & 0x02U) != 0U;
    decoded->address = request & 0x0cU;
    return true;
}

static uint8_t backend_transfer(void *ctx, uint8_t request, uint32_t *data)
{
    struct gpio_dap_backend *backend = ctx;
    struct swd_request decoded = {0};
    if (backend == NULL || !backend->connected ||
        !decode_request(request, &decoded)) {
        return 7U;
    }
    enum swd_ack ack = SWD_ACK_INVALID;
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        ack = swd_transfer(&backend->probe,
                           decoded,
                           decoded.read ? 0U : *data,
                           decoded.read ? data : NULL);
        if (ack != SWD_ACK_WAIT) {
            break;
        }
    }
    if (ack == SWD_ACK_OK && decoded.ap && !decoded.read &&
        decoded.address == 4U) {
        backend->tar = *data;
        backend->tar_valid = true;
    }
    return dap_ack(ack);
}

static uint8_t backend_transfer_block(void *ctx,
                                      uint8_t request,
                                      uint16_t count,
                                      uint32_t *data)
{
    struct gpio_dap_backend *backend = ctx;
    struct swd_request decoded = {0};
    if (backend == NULL || !backend->connected ||
        !decode_request(request, &decoded)) {
        return 7U;
    }
    if (decoded.ap && decoded.read && decoded.address == 12U &&
        backend->tar_valid && count != 0U &&
        gpio_mmio_fast_read_block(&backend->gpio,
                                  data,
                                  (size_t)count * sizeof(uint32_t),
                                  NULL)) {
        backend->tar += (uint32_t)count * UINT32_C(4);
        return 1U;
    }
    for (uint16_t index = 0U; index < count; ++index) {
        uint8_t status = backend_transfer(backend, request, &data[index]);
        if (status != 1U) {
            return status;
        }
    }
    return 1U;
}

static bool backend_connect(void *ctx, uint8_t port)
{
    struct gpio_dap_backend *backend = ctx;
    if (backend == NULL || !backend->open || port != 1U) {
        return false;
    }
    bool success = swd_switch_to_swd(&backend->probe);
    backend->connected = success;
    backend->tar_valid = false;
    return success;
}

static void backend_disconnect(void *ctx)
{
    struct gpio_dap_backend *backend = ctx;
    if (backend != NULL) {
        backend->connected = false;
        backend->tar_valid = false;
    }
}

static bool backend_set_clock(void *ctx, uint32_t clock_hz)
{
    struct gpio_dap_backend *backend = ctx;
    if (backend == NULL || !backend->open || clock_hz == 0U) {
        return false;
    }
    backend->gpio.half_cycle_ns = 1000000000UL /
                                  ((unsigned long)clock_hz * 2UL);
    if (backend->gpio.half_cycle_ns == 0UL) {
        backend->gpio.half_cycle_ns = 1UL;
    }
    return true;
}

static bool backend_swj_sequence(void *ctx,
                                 unsigned bit_count,
                                 const uint8_t *bits)
{
    struct gpio_dap_backend *backend = ctx;
    if (backend == NULL || !backend->connected || bits == NULL) {
        return false;
    }
    backend->probe.io.set_swdio_output(backend->probe.io.ctx, true);
    for (unsigned bit = 0U; bit < bit_count; ++bit) {
        bool value = ((bits[bit / 8U] >> (bit % 8U)) & 1U) != 0U;
        backend->probe.io.write_bit_cycle(backend->probe.io.ctx, value);
    }
    return true;
}

static bool backend_swd_configure(void *ctx, const uint8_t *configuration)
{
    (void)ctx;
    (void)configuration;
    /* The GPIO backend uses the target's normal SWD turnaround timing. */
    return true;
}

static uint8_t backend_swj_pins(void *ctx,
                                uint8_t output,
                                uint8_t mask,
                                uint32_t delay_us)
{
    struct gpio_dap_backend *backend = ctx;
    (void)mask;
    (void)delay_us;
    if (backend == NULL || !backend->open) {
        return 0U;
    }
    /* This probe has no reset connection. SWCLK/SWDIO are managed by the
     * SWD transaction engine, so acknowledge the request without changing
     * their direction outside a transaction. */
    return output;
}

int gpio_dap_open(struct gpio_dap_backend *backend,
                  unsigned swclk_pin,
                  unsigned swdio_pin,
                  unsigned clock_hz)
{
    if (backend == NULL) {
        return EINVAL;
    }
    memset(backend, 0, sizeof(*backend));
    int result = gpio_mmio_open(&backend->gpio,
                                swclk_pin,
                                swdio_pin,
                                clock_hz);
    if (result != 0) {
        return result;
    }
    backend->probe.io.ctx = &backend->gpio;
    backend->probe.io.set_swdio_output = gpio_mmio_set_swdio_output;
    backend->probe.io.write_swdio = gpio_mmio_write_swdio;
    backend->probe.io.read_swdio = gpio_mmio_read_swdio;
    backend->probe.io.write_swclk = gpio_mmio_write_swclk;
    backend->probe.io.wait_half_cycle = gpio_mmio_wait_half_cycle;
    backend->probe.io.clock_cycle = gpio_mmio_clock_cycle;
    backend->probe.io.write_bit_cycle = gpio_mmio_write_bit_cycle;
    backend->probe.io.read_bit_cycle = gpio_mmio_read_bit_cycle;
    backend->probe.io.write_bits = gpio_mmio_write_bits;
    backend->probe.io.read_bits = gpio_mmio_read_bits;
    backend->probe.retry_limit = 8U;
    backend->open = true;
    return 0;
}

void gpio_dap_close(struct gpio_dap_backend *backend)
{
    if (backend != NULL && backend->open) {
        gpio_mmio_close(&backend->gpio);
        backend->open = false;
        backend->connected = false;
        backend->tar_valid = false;
    }
}

bool gpio_dap_attach(struct gpio_dap_backend *backend, uint32_t *idcode)
{
    if (backend == NULL || !backend->open) {
        return false;
    }
    if (!swd_switch_to_swd(&backend->probe)) {
        return false;
    }
    backend->connected = true;
    backend->tar_valid = false;

    uint32_t observed_idcode = 0U;
    bool success = swd_read_idcode(&backend->probe, &observed_idcode) &&
                   swd_prepare_memory_access(&backend->probe);
    if (!success) {
        backend->connected = false;
        return false;
    }
    if (idcode != NULL) {
        *idcode = observed_idcode;
    }
    return true;
}

void gpio_dap_detach(struct gpio_dap_backend *backend)
{
    if (backend != NULL) {
        backend->connected = false;
        backend->tar_valid = false;
    }
}

bool gpio_dap_read_memory(struct gpio_dap_backend *backend,
                          uint32_t address,
                          void *buffer,
                          size_t length)
{
    if (backend == NULL || !backend->connected || buffer == NULL ||
        length == 0U || (length % sizeof(uint32_t)) != 0U) {
        return false;
    }
    /* The STM32 MEM-AP TAR auto-increment is only reliable within a 1 KiB
     * window. DAPJS works around this explicitly; a native GDB request can be
     * much larger (the stub verifier asks for 16 KiB), so split it here and
     * reprogram TAR at every boundary. Without this, the first 1024 bytes of
     * a read are correct and the next window can come from the wrong address. */
    uint8_t *destination = (uint8_t *)buffer;
    size_t offset = 0U;
    while (offset < length) {
        uint32_t window_address = address + (uint32_t)offset;
        size_t window = 1024U - ((size_t)window_address & 1023U);
        if (window > length - offset) {
            window = length - offset;
        }
        if (!gpio_mmio_fast_read_memory(&backend->gpio,
                                        window_address,
                                        destination + offset,
                                        window,
                                        NULL)) {
            backend->tar_valid = false;
            /* GDB legitimately probes unmapped stack/instruction addresses.
             * Return the failed read, but clear sticky MEM-AP faults so the
             * next valid request works without reconnecting or resetting. */
            uint32_t abort_bits = 0x1eU;
            (void)backend_transfer(backend, 0U, &abort_bits);
            return false;
        }
        offset += window;
    }
    backend->tar = address + (uint32_t)length;
    backend->tar_valid = true;
    return true;
}

static bool transfer_word(struct gpio_dap_backend *backend,
                          struct swd_request request,
                          uint32_t write_data,
                          uint32_t *read_data)
{
    for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
        enum swd_ack ack = swd_transfer(&backend->probe,
                                        request,
                                        write_data,
                                        read_data);
        if (ack == SWD_ACK_OK) {
            return true;
        }
        if (ack != SWD_ACK_WAIT) {
            return false;
        }
    }
    return false;
}

static bool read_word(struct gpio_dap_backend *backend,
                      uint32_t address,
                      uint32_t *value)
{
    return gpio_mmio_fast_read_memory(&backend->gpio,
                                      address,
                                      value,
                                      sizeof(*value),
                                      NULL);
}

static bool write_word(struct gpio_dap_backend *backend,
                       uint32_t address,
                       uint32_t value)
{
    if (!transfer_word(backend,
                       (struct swd_request){.ap = true, .read = false, .address = 4U},
                       address,
                       NULL) ||
        !transfer_word(backend,
                       (struct swd_request){.ap = true, .read = false, .address = 12U},
                       value,
                       NULL)) {
        return false;
    }
    uint32_t discard = 0U;
    return transfer_word(backend,
                         (struct swd_request){.ap = false, .read = true, .address = 12U},
                         0U,
                         &discard);
}

static bool wait_core_register_ready(struct gpio_dap_backend *backend)
{
    for (unsigned attempt = 0U; attempt < 128U; ++attempt) {
        uint32_t status = 0U;
        if (!read_word(backend, cortex_m_dhcsr, &status)) {
            return false;
        }
        if ((status & cortex_m_dhcsr_reg_ready) != 0U) {
            return true;
        }
    }
    return false;
}

bool gpio_dap_write_memory(struct gpio_dap_backend *backend,
                           uint32_t address,
                           const void *buffer,
                           size_t length)
{
    if (backend == NULL || !backend->connected || buffer == NULL) {
        return false;
    }
    if (length == 0U) {
        return true;
    }

    backend->tar_valid = false;

    const uint8_t *source = (const uint8_t *)buffer;
    size_t offset = 0U;
    while (offset < length) {
        uint32_t current = address + (uint32_t)offset;
        size_t window = 1024U - ((size_t)current & 1023U);
        if (window > length - offset) {
            window = length - offset;
        }

        if ((current & 3U) != 0U || window < sizeof(uint32_t)) {
            uint32_t word_address = current & ~UINT32_C(3);
            size_t in_word = (size_t)current & 3U;
            size_t count = sizeof(uint32_t) - in_word;
            if (count > window) {
                count = window;
            }
            uint32_t value = 0U;
            if (!read_word(backend, word_address, &value)) {
                return false;
            }
            memcpy((uint8_t *)&value + in_word, source + offset, count);
            if (!write_word(backend, word_address, value)) {
                return false;
            }
            offset += count;
            continue;
        }

        /* Keep the write-side completion conservative.  An AP DRW write is
         * posted on this target: an ACK for the DRW request does not by
         * itself prove that the previous write has reached SRAM.  The old
         * per-word path drained RDBUFF after every word and was reliable for
         * loading executable RAM.  A single drain per 1 KiB block can leave
         * the RAM stub partially corrupted at high SWCLK rates, producing
         * random GDB verify mismatches. */
        uint32_t value = 0U;
        memcpy(&value, source + offset, sizeof(value));
        if (!write_word(backend, current, value)) {
            backend->tar_valid = false;
            return false;
        }
        offset += sizeof(value);
    }
    return true;
}

bool gpio_dap_read_core_register(struct gpio_dap_backend *backend,
                                 unsigned index,
                                 uint32_t *value)
{
    if (backend == NULL || !backend->connected || value == NULL || index > 16U) {
        return false;
    }
    if (!write_word(backend, cortex_m_dcrsr, index)) {
        return false;
    }
    return wait_core_register_ready(backend) &&
           read_word(backend, cortex_m_dcrdr, value);
}

bool gpio_dap_read_m_system_register(struct gpio_dap_backend *backend,
                                     unsigned index,
                                     uint32_t *value)
{
    if (backend == NULL || !backend->connected || value == NULL) {
        return false;
    }

    unsigned selector = index;
    unsigned shift = 0U;
    uint32_t mask = UINT32_MAX;
    if (index >= gpio_dap_primask_register &&
        index <= gpio_dap_control_register) {
        selector = 20U;
        shift = (index & 3U) * 8U;
        mask = UINT32_C(0xff);
    } else if (index != gpio_dap_msp_register &&
               index != gpio_dap_psp_register) {
        return false;
    }

    if (!write_word(backend, cortex_m_dcrsr, selector) ||
        !wait_core_register_ready(backend)) {
        return false;
    }
    uint32_t raw = 0U;
    if (!read_word(backend, cortex_m_dcrdr, &raw)) {
        return false;
    }
    *value = (raw >> shift) & mask;
    return true;
}

bool gpio_dap_write_core_register(struct gpio_dap_backend *backend,
                                  unsigned index,
                                  uint32_t value)
{
    if (backend == NULL || !backend->connected || index > 16U) {
        return false;
    }
    return write_word(backend, cortex_m_dcrdr, value) &&
           write_word(backend, cortex_m_dcrsr, index | cortex_m_dcrsr_write) &&
           wait_core_register_ready(backend);
}

bool gpio_dap_halt(struct gpio_dap_backend *backend)
{
    /* STM32H7 watchdogs run asynchronously. Freeze them during debug halt,
     * as the probe transport previously did in the browser. These bits affect
     * halted execution only, not a running firmware's watchdog protection. */
    const uint32_t freeze_addresses[] = {0x5c001034U, 0x5c001054U};
    const uint32_t freeze_masks[] = {1U << 6, 1U << 18};
    for (unsigned i = 0U; i < 2U; ++i) {
        uint32_t value = 0U;
        if (read_word(backend, freeze_addresses[i], &value)) {
            (void)write_word(backend, freeze_addresses[i], value | freeze_masks[i]);
        }
    }
    if (!write_word(backend, cortex_m_dhcsr, cortex_m_debug_key | 0x3U)) {
        return false;
    }
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        uint32_t status = 0U;
        if (!read_word(backend, cortex_m_dhcsr, &status)) {
            return false;
        }
        if ((status & (1U << 17)) != 0U) {
            return true;
        }
        const struct timespec pause = {.tv_nsec = 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    return false;
}

bool gpio_dap_resume(struct gpio_dap_backend *backend)
{
    return write_word(backend, cortex_m_dhcsr, cortex_m_debug_key | 0x1U);
}

bool gpio_dap_reset(struct gpio_dap_backend *backend)
{
    uint32_t demcr = 0U;
    if (!gpio_dap_halt(backend) ||
        !read_word(backend, cortex_m_demcr, &demcr) ||
        !write_word(backend, cortex_m_demcr, demcr | 1U)) {
        return false;
    }
    /* Reset may complete after the posted-write drain. Catch it before the
     * client's continue/halt sequence can race a still-pending reset. */
    (void)write_word(backend, cortex_m_aircr, 0x05fa0004U);
    const struct timespec settle = {.tv_nsec = 50000000L};
    (void)nanosleep(&settle, NULL);
    gpio_dap_detach(backend);
    if (!gpio_dap_attach(backend, NULL)) {
        return false;
    }
    uint32_t status = 0U;
    bool halted = read_word(backend, cortex_m_dhcsr, &status) &&
                  (status & (1U << 17)) != 0U;
    bool restored = write_word(backend, cortex_m_demcr, demcr);
    return halted && restored;
}

struct cmsis_dap_backend gpio_dap_make_backend(struct gpio_dap_backend *backend)
{
    return (struct cmsis_dap_backend){
        .ctx = backend,
        .connect = backend_connect,
        .disconnect = backend_disconnect,
        .set_clock = backend_set_clock,
        .swj_sequence = backend_swj_sequence,
        .swd_configure = backend_swd_configure,
        .swj_pins = backend_swj_pins,
        .transfer = backend_transfer,
        .transfer_block = backend_transfer_block,
    };
}
