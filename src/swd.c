#include "swd.h"

#include <stdio.h>
#include <string.h>

static bool odd_parity32(uint32_t value)
{
    return __builtin_parity(value) != 0;
}

static void clock_pulse(struct swd_probe *probe)
{
    if (probe->io.clock_cycle != NULL) {
        probe->io.clock_cycle(probe->io.ctx);
        return;
    }
    probe->io.write_swclk(probe->io.ctx, false);
    probe->io.wait_half_cycle(probe->io.ctx);
    probe->io.write_swclk(probe->io.ctx, true);
    probe->io.wait_half_cycle(probe->io.ctx);
}

static void write_bit(struct swd_probe *probe, bool bit)
{
    if (probe->io.write_bit_cycle != NULL) {
        probe->io.write_bit_cycle(probe->io.ctx, bit);
        return;
    }
    probe->io.write_swdio(probe->io.ctx, bit);
    clock_pulse(probe);
}

static bool read_bit(struct swd_probe *probe)
{
    if (probe->io.read_bit_cycle != NULL) {
        return probe->io.read_bit_cycle(probe->io.ctx);
    }
    probe->io.write_swclk(probe->io.ctx, false);
    probe->io.wait_half_cycle(probe->io.ctx);
    bool bit = probe->io.read_swdio(probe->io.ctx);
    probe->io.write_swclk(probe->io.ctx, true);
    probe->io.wait_half_cycle(probe->io.ctx);
    return bit;
}

static void write_bits(struct swd_probe *probe, uint32_t value, unsigned count)
{
    if (probe->io.write_bits != NULL) {
        probe->io.write_bits(probe->io.ctx, value, count);
        return;
    }
    for (unsigned bit = 0U; bit < count; ++bit) {
        write_bit(probe, ((value >> bit) & 1U) != 0U);
    }
}

static uint32_t read_bits(struct swd_probe *probe, unsigned count)
{
    if (probe->io.read_bits != NULL) {
        return probe->io.read_bits(probe->io.ctx, count);
    }
    uint32_t value = 0U;
    for (unsigned bit = 0U; bit < count; ++bit) {
        if (read_bit(probe)) {
            value |= 1U << bit;
        }
    }
    return value;
}

static void turnaround_to_input(struct swd_probe *probe)
{
    probe->io.set_swdio_output(probe->io.ctx, false);
    clock_pulse(probe);
}

static void turnaround_to_output(struct swd_probe *probe)
{
    clock_pulse(probe);
    probe->io.set_swdio_output(probe->io.ctx, true);
    probe->io.write_swdio(probe->io.ctx, true);
}

uint8_t swd_encode_request(struct swd_request request)
{
    uint8_t value = 0x81U;
    if (request.ap) {
        value |= 0x02U;
    }
    if (request.read) {
        value |= 0x04U;
    }
    /* A[3:2] occupy request bits [4:3], so shift the register address. */
    value |= (uint8_t)((request.address & 0x0cU) << 1U);

    unsigned ones = 0U;
    for (unsigned bit = 1U; bit <= 4U; ++bit) {
        ones += (unsigned)((value >> bit) & 1U);
    }
    if ((ones & 1U) != 0U) {
        value |= 0x20U;
    }
    return value;
}

bool swd_request_parity_is_valid(uint8_t request_byte)
{
    unsigned ones = 0U;
    for (unsigned bit = 1U; bit <= 5U; ++bit) {
        ones += (unsigned)((request_byte >> bit) & 1U);
    }
    return (ones & 1U) == 0U;
}

void swd_line_reset(struct swd_probe *probe)
{
    probe->io.set_swdio_output(probe->io.ctx, true);
    for (unsigned bit = 0U; bit < 56U; ++bit) {
        write_bit(probe, true);
    }
}

bool swd_switch_to_swd(struct swd_probe *probe)
{
    swd_line_reset(probe);
    write_bits(probe, 0xe79eU, 16U);
    swd_line_reset(probe);
    /* The SWJ sequence ends with two low idle cycles before the first request. */
    write_bit(probe, false);
    write_bit(probe, false);

    uint32_t idcode = 0U;
    return swd_transfer(probe,
                        (struct swd_request){.ap = false, .read = true, .address = 0U},
                        0U,
                        &idcode) == SWD_ACK_OK;
}

static enum swd_ack swd_transfer_encoded(struct swd_probe *probe,
                                         struct swd_request request,
                                         uint8_t request_byte,
                                         uint32_t write_data,
                                         uint32_t *read_data)
{
    if (probe->debug) {
        fprintf(stderr,
                "SWD request=0x%02x ap=%u read=%u addr=0x%02x\n",
                request_byte,
                request.ap ? 1U : 0U,
                request.read ? 1U : 0U,
                request.address);
    }
    probe->io.set_swdio_output(probe->io.ctx, true);
    write_bits(probe, request_byte, 8U);
    turnaround_to_input(probe);

    enum swd_ack ack = (enum swd_ack)read_bits(probe, 3U);
    if (probe->debug) {
        fprintf(stderr, "SWD ack=0x%x\n", (unsigned)ack);
    }
    if (ack != SWD_ACK_OK) {
        turnaround_to_output(probe);
        return ack;
    }

    if (request.read) {
        uint32_t value = read_bits(probe, 32U);
        bool parity = read_bit(probe);
        if (probe->debug) {
            fprintf(stderr,
                    "SWD read value=0x%08x parity=%u expected=%u ones=%u\n",
                    value,
                    parity ? 1U : 0U,
                    odd_parity32(value) ? 1U : 0U,
                    __builtin_popcount(value));
        }
        if (odd_parity32(value) != parity) {
            turnaround_to_output(probe);
            return SWD_ACK_INVALID;
        }
        if (read_data != NULL) {
            *read_data = value;
        }
        turnaround_to_output(probe);
    } else {
        turnaround_to_output(probe);
        write_bits(probe, write_data, 32U);
        write_bit(probe, odd_parity32(write_data));
    }
    return SWD_ACK_OK;
}

enum swd_ack swd_transfer(struct swd_probe *probe,
                          struct swd_request request,
                          uint32_t write_data,
                          uint32_t *read_data)
{
    return swd_transfer_encoded(probe,
                                request,
                                swd_encode_request(request),
                                write_data,
                                read_data);
}

static bool transfer_encoded_with_retries(struct swd_probe *probe,
                                          struct swd_request request,
                                          uint8_t request_byte,
                                          uint32_t write_data,
                                          uint32_t *read_data)
{
    unsigned attempts = 0U;
    while (attempts++ <= probe->retry_limit) {
        enum swd_ack ack = swd_transfer_encoded(probe,
                                                request,
                                                request_byte,
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

static bool transfer_with_retries(struct swd_probe *probe,
                                  struct swd_request request,
                                  uint32_t write_data,
                                  uint32_t *read_data)
{
    return transfer_encoded_with_retries(probe,
                                         request,
                                         swd_encode_request(request),
                                         write_data,
                                         read_data);
}

bool swd_read_idcode(struct swd_probe *probe, uint32_t *idcode)
{
    return transfer_with_retries(probe,
                                 (struct swd_request){.ap = false, .read = true, .address = 0U},
                                 0U,
                                 idcode);
}

bool swd_prepare_memory_access(struct swd_probe *probe)
{
    uint32_t status = 0U;
    if (!transfer_with_retries(probe,
                               (struct swd_request){.ap = false, .read = false, .address = 0U},
                               0x0000001eU,
                               NULL)) {
        return false;
    }
    if (!transfer_with_retries(probe,
                               (struct swd_request){.ap = false, .read = false, .address = 4U},
                               0x50000000U,
                               NULL)) {
        return false;
    }
    for (unsigned attempt = 0U; attempt < 100U; ++attempt) {
        if (!transfer_with_retries(probe,
                                   (struct swd_request){.ap = false, .read = true, .address = 4U},
                                   0U,
                                   &status)) {
            return false;
        }
        if ((status & 0xa0000000U) == 0xa0000000U) {
            break;
        }
    }
    if ((status & 0xa0000000U) != 0xa0000000U) {
        return false;
    }

    if (!transfer_with_retries(probe,
                               (struct swd_request){.ap = false, .read = false, .address = 8U},
                               0U,
                               NULL)) {
        return false;
    }
    return transfer_with_retries(probe,
                                 (struct swd_request){.ap = true, .read = false, .address = 0U},
                                 0x23000052U,
                                 NULL);
}

bool swd_read_memory(struct swd_probe *probe,
                     uint32_t address,
                     void *buffer,
                     size_t length,
                     size_t *bytes_read)
{
    if (buffer == NULL || (length % 4U) != 0U) {
        return false;
    }
    if (bytes_read != NULL) {
        *bytes_read = 0U;
    }

    if (!transfer_with_retries(probe,
                               (struct swd_request){.ap = true, .read = false, .address = 4U},
                               address,
                               NULL)) {
        return false;
    }

    size_t words = length / 4U;
    const struct swd_request ap_read_request = {
        .ap = true, .read = true, .address = 12U};
    const struct swd_request dp_read_request = {
        .ap = false, .read = true, .address = 12U};
    uint8_t ap_read_byte = swd_encode_request(ap_read_request);
    uint8_t dp_read_byte = swd_encode_request(dp_read_request);
    uint32_t discard = 0U;
    if (words == 0U || !transfer_with_retries(probe,
                                              ap_read_request,
                                              0U,
                                              &discard)) {
        return words == 0U;
    }

    uint8_t *destination = (uint8_t *)buffer;
    for (size_t word = 0U; word < words; ++word) {
        uint32_t value = 0U;
        if (word + 1U < words) {
            if (!transfer_encoded_with_retries(probe,
                                               ap_read_request,
                                               ap_read_byte,
                                               0U,
                                               &value)) {
                return false;
            }
        } else if (!transfer_encoded_with_retries(probe,
                                                  dp_read_request,
                                                  dp_read_byte,
                                                  0U,
                                                  &value)) {
            return false;
        }
        memcpy(destination + word * 4U, &value, sizeof(value));
        if (bytes_read != NULL) {
            *bytes_read += 4U;
        }
    }
    return true;
}
