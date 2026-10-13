#ifndef RPI_FAST_GPIO_DAP_H
#define RPI_FAST_GPIO_DAP_H

#include "cmsis_dap.h"
#include "gpio_mmio.h"
#include "swd.h"

#include <stdbool.h>
#include <stdint.h>

/* Linux GPIO/SWD backend for the CMSIS-DAP command processor. The backend
 * keeps ordinary transfers available for setup and uses the fused MMIO
 * reader for AP DRW TransferBlock reads after a TAR write. */
struct gpio_dap_backend {
    struct gpio_mmio gpio;
    struct swd_probe probe;
    bool open;
    bool connected;
    bool tar_valid;
    uint32_t tar;
};

int gpio_dap_open(struct gpio_dap_backend *backend,
                  unsigned swclk_pin,
                  unsigned swdio_pin,
                  unsigned clock_hz);
void gpio_dap_close(struct gpio_dap_backend *backend);

/* Attach the target for a native host-side adapter. This performs SWD
 * activation, validates the DP IDCODE, powers the debug/system domains, and
 * selects a 32-bit incrementing MEM-AP access. */
bool gpio_dap_attach(struct gpio_dap_backend *backend, uint32_t *idcode);
void gpio_dap_detach(struct gpio_dap_backend *backend);

/* Memory operations for native transports such as the GDB server. Reads and
 * aligned writes use fused MEM-AP streams; writes still support arbitrary byte
 * ranges through read/modify/write at the unaligned boundaries. */
bool gpio_dap_read_memory(struct gpio_dap_backend *backend,
                          uint32_t address,
                          void *buffer,
                          size_t length);
bool gpio_dap_write_memory(struct gpio_dap_backend *backend,
                           uint32_t address,
                           const void *buffer,
                           size_t length);

bool gpio_dap_read_core_register(struct gpio_dap_backend *backend,
                                 unsigned index,
                                 uint32_t *value);
bool gpio_dap_write_core_register(struct gpio_dap_backend *backend,
                                  unsigned index,
                                  uint32_t value);
bool gpio_dap_halt(struct gpio_dap_backend *backend);
bool gpio_dap_resume(struct gpio_dap_backend *backend);
bool gpio_dap_reset(struct gpio_dap_backend *backend);

struct cmsis_dap_backend gpio_dap_make_backend(struct gpio_dap_backend *backend);

#endif
