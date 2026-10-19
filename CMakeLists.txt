cmake_minimum_required(VERSION 3.16)
project(gwmgr_fast_pi_gpio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(GWMGR_FAST_PI_GPIO_RELAXED_BARRIERS
       "Use the validated ARM store/load barrier split"
       ON)
option(GWMGR_FAST_PI_GPIO_LTO
       "Enable link-time optimization for the probe binaries"
       ON)
option(GWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS
       "Omit the fused-reader barrier after a rising SWCLK edge"
       ON)
option(GWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS
       "Omit the fused-reader barrier between SWCLK-low and input sampling"
       ON)
option(GWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS
       "Omit fused-reader barriers after SWDIO data writes"
       ON)
option(GWMGR_FAST_PI_GPIO_A72_TUNING
       "Tune ARM builds for the Raspberry Pi 4 Cortex-A72"
       ON)
set(GWMGR_FAST_PI_GPIO_CMSIS_DAP_PACKET_SIZE 1024 CACHE STRING
    "CMSIS-DAP packet size in bytes (use 4096 for native TCP experiments)")

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
add_compile_definitions(
    GWMGR_FAST_PI_GPIO_CMSIS_DAP_PACKET_SIZE=${GWMGR_FAST_PI_GPIO_CMSIS_DAP_PACKET_SIZE}
)

if(GWMGR_FAST_PI_GPIO_A72_TUNING AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    add_compile_options(-mcpu=cortex-a72 -mtune=cortex-a72)
endif()

if(GWMGR_FAST_PI_GPIO_LTO)
    add_compile_options(-flto)
    add_link_options(-flto)
endif()

if(GWMGR_FAST_PI_GPIO_RELAXED_BARRIERS)
    add_compile_definitions(
        GWMGR_FAST_PI_GPIO_STORE_BARRIERS
        GWMGR_FAST_PI_GPIO_READ_BARRIERS
    )
endif()

if(GWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS)
    add_compile_definitions(GWMGR_FAST_PI_GPIO_NO_CLOCK_RISE_BARRIERS)
endif()

if(GWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS)
    add_compile_definitions(GWMGR_FAST_PI_GPIO_NO_INPUT_READ_BARRIERS)
endif()

if(GWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS)
    add_compile_definitions(GWMGR_FAST_PI_GPIO_NO_SWDIO_WRITE_BARRIERS)
endif()

add_library(swd_protocol
    src/swd.c
)
target_include_directories(swd_protocol PUBLIC include)
target_link_libraries(swd_protocol PUBLIC rt)

add_library(cmsis_dap
    src/cmsis_dap.c
)
target_include_directories(cmsis_dap PUBLIC include)

add_library(gpio_dap
    src/gpio_dap.c
    src/gpio_mmio.c
)
target_include_directories(gpio_dap PUBLIC include)
target_link_libraries(gpio_dap PUBLIC cmsis_dap swd_protocol)

add_executable(swd-read
    src/main.c
    src/gpio_mmio.c
)
target_include_directories(swd-read PRIVATE include)
target_link_libraries(swd-read PRIVATE swd_protocol)

add_executable(packed-read
    src/packed_read.c
    src/gpio_mmio.c
)
target_include_directories(packed-read PRIVATE include)
target_link_libraries(packed-read PRIVATE swd_protocol)

add_executable(cmsis-dap-read
    src/cmsis_dap_read.c
)
target_link_libraries(cmsis-dap-read PRIVATE gpio_dap)

add_executable(cmsis-dap-functionfs
    src/cmsis_dap_functionfs.c
)
target_link_libraries(cmsis-dap-functionfs PRIVATE gpio_dap)

add_executable(cmsis-dap-tcp
    src/cmsis_dap_tcp.c
)
target_link_libraries(cmsis-dap-tcp PRIVATE gpio_dap)

add_executable(gdb-swd
    src/gdb_swd.c
)
target_link_libraries(gdb-swd PRIVATE gpio_dap)

add_executable(gpio-dap-read-loop
    src/gpio_dap_read_loop.c
)
target_link_libraries(gpio-dap-read-loop PRIVATE gpio_dap)

add_executable(dma-waveform
    src/dma_waveform.c
)

add_executable(dma-swd-idcode
    src/dma_swd_idcode.c
)

add_executable(dma-stream-read
    src/dma_stream_read.c
    src/gpio_mmio.c
)
target_include_directories(dma-stream-read PRIVATE include)
target_link_libraries(dma-stream-read PRIVATE swd_protocol)

add_executable(dma-parallel-read
    src/dma_parallel_read.c
    src/gpio_mmio.c
)
target_include_directories(dma-parallel-read PRIVATE include)
target_link_libraries(dma-parallel-read PRIVATE swd_protocol)

add_executable(spi-swd-probe
    src/spi_swd_probe.c
)

add_executable(spi3wire-idcode
    src/spi3wire_idcode.c
)

add_executable(spi3wire-burst-read
    src/spi3wire_burst_read.c
    src/gpio_mmio.c
)
target_include_directories(spi3wire-burst-read PRIVATE include)
target_link_libraries(spi3wire-burst-read PRIVATE swd_protocol)

add_executable(spi-swd-memory
    src/spi_swd_memory.c
)

add_executable(spi-swd-burst-read
    src/spi_swd_burst_read.c
    src/gpio_mmio.c
)
target_include_directories(spi-swd-burst-read PRIVATE include)
target_link_libraries(spi-swd-burst-read PRIVATE swd_protocol)

add_executable(spi-ren-memory
    src/spi_ren_memory.c
)

add_executable(spi-pipelined-read
    src/spi_pipelined_read.c
    src/gpio_mmio.c
)
target_include_directories(spi-pipelined-read PRIVATE include)
target_link_libraries(spi-pipelined-read PRIVATE swd_protocol)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_gdb_protocol tests/test_gdb_protocol.c)
    target_include_directories(test_gdb_protocol PRIVATE include)
    target_link_libraries(test_gdb_protocol PRIVATE gpio_dap)
    add_test(NAME gdb_protocol COMMAND test_gdb_protocol)

    add_executable(test_swd_protocol tests/test_swd_protocol.c)
    target_include_directories(test_swd_protocol PRIVATE include)
    target_link_libraries(test_swd_protocol PRIVATE swd_protocol)
    add_test(NAME swd_protocol COMMAND test_swd_protocol)

    add_executable(test_cmsis_dap tests/test_cmsis_dap.c)
    target_include_directories(test_cmsis_dap PRIVATE include)
    target_link_libraries(test_cmsis_dap PRIVATE cmsis_dap)
    add_test(NAME cmsis_dap COMMAND test_cmsis_dap)
endif()
