/**
 * ZuluIDE™ - Copyright (c) 2023 Rabbit Hole Computing™
 *
 * ZuluIDE™ firmware is licensed under the GPL version 3 or any later version. 
 *
 * https://www.gnu.org/licenses/gpl-3.0.html
 * ----
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. 
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
**/

#include <ide_phy.h>
#include <ide_constants.h>
#include "rp2040_fpga.h"
#include <assert.h>
#include <ZuluIDE_log.h>

static struct {
    ide_phy_config_t config;
    bool transfer_running;
    int udma_mode;
    volatile bool watchdog_error;

    int crc_errors;
    uint32_t block_crc0;
    uint32_t block_crc1;
} g_ide_phy;

#define BLOCK_CRC_VALID 0x10000

// Compare the CRC we calculated with DMA when writing to FPGA
// against the CRC received from the host in UltraDMA mode.
static void verify_crc(uint32_t *block_crc)
{
    uint16_t host_crc = 0;
    fpga_rdcmd(FPGA_CMD_READ_UDMA_CRC, (uint8_t*)&host_crc, 2);

    if (!(*block_crc & BLOCK_CRC_VALID))
    {
        // This was a 1-block transfer, the READ_UDMA_CRC command above
        // is still needed to read out the block CRCs from FPGA.
    }
    else if ((uint16_t)*block_crc != host_crc)
    {
        logmsg("WARNING: UltraDMA CRC mismatch, calculated ", (uint16_t)*block_crc, ", host sent ", host_crc);
        g_ide_phy.crc_errors++;
    }

    *block_crc = 0;
}

static ide_phy_capabilities_t g_ide_phy_capabilities = {
    // ICE5LP1K has 8 kB of RAM, we use it as 2x 4096 byte buffers
    .max_blocksize = 4096,

    .supports_iordy = true,
    .max_pio_mode = 2, // PIO3 seems to have timing problems
    .min_pio_cycletime_no_iordy = 240,
    .min_pio_cycletime_with_iordy = 180,

    .max_udma_mode = 0,
};

// Reset the IDE phy
void ide_phy_reset(const ide_phy_config_t* config)
{
    g_ide_phy.config = *config;
    g_ide_phy.watchdog_error = false;

    fpga_init();

    uint8_t cfg = 0;
    if (config->enable_dev0)       cfg |= 0x01;
    if (config->enable_dev1)       cfg |= 0x02;
    if (config->enable_dev1_zeros) cfg |= 0x04;
    if (config->atapi_dev0)        cfg |= 0x08;
    if (config->atapi_dev1)        cfg |= 0x10;
    fpga_wrcmd(FPGA_CMD_SET_IDE_PHY_CFG, &cfg, 1);
}

void ide_phy_reset_from_watchdog()
{
    g_ide_phy.watchdog_error = true;
}

// Poll for new events.
// Returns IDE_EVENT_NONE if no new events.
ide_event_t ide_phy_get_events()
{
    uint8_t status;
    fpga_rdcmd(FPGA_CMD_READ_STATUS, &status, 1);

    if (g_ide_phy.watchdog_error)
    {
        ide_phy_reset(&g_ide_phy.config);
        return IDE_EVENT_HWRST;
    }
    else if (status & FPGA_STATUS_IDE_RST)
    {
        uint8_t clrmask = FPGA_STATUS_IDE_RST;
        fpga_wrcmd(FPGA_CMD_CLR_IRQ_FLAGS, &clrmask, 1);
        return IDE_EVENT_HWRST;
    }
    else if (status & FPGA_STATUS_IDE_SRST)
    {
        // Check if software reset state has ended
        ide_registers_t regs;
        fpga_rdcmd(FPGA_CMD_READ_IDE_REGS, (uint8_t*)&regs, sizeof(regs));

        if (!(regs.device_control & IDE_DEVCTRL_SRST))
        {
            uint8_t clrmask = FPGA_STATUS_IDE_SRST;
            fpga_wrcmd(FPGA_CMD_CLR_IRQ_FLAGS, &clrmask, 1);
            return IDE_EVENT_SWRST;
        }
    }
    else if (status & FPGA_STATUS_IDE_CMD)
    {
        uint8_t clrmask = FPGA_STATUS_IDE_CMD;
        fpga_wrcmd(FPGA_CMD_CLR_IRQ_FLAGS, &clrmask, 1);
        return IDE_EVENT_CMD;
    }
    else if (g_ide_phy.transfer_running)
    {
        if (status & FPGA_STATUS_DATA_DIR)
        {
            if (status & FPGA_STATUS_TX_DONE)
            {
                g_ide_phy.transfer_running = false;
                return IDE_EVENT_DATA_TRANSFER_DONE;
            }
        }
        else
        {
            if (status & FPGA_STATUS_RX_DONE)
            {
                g_ide_phy.transfer_running = false;
                return IDE_EVENT_DATA_TRANSFER_DONE;
            }
        }
    }
    
    return IDE_EVENT_NONE;
}

// Get current state of IDE registers
void ide_phy_get_regs(ide_registers_t *regs)
{
    fpga_rdcmd(FPGA_CMD_READ_IDE_REGS, (uint8_t*)regs, sizeof(*regs));
    // dbgmsg("ide_phy_get_regs(", bytearray((const uint8_t*)regs, sizeof(*regs)), ")");
}

// Set current state of IDE registers
void ide_phy_set_regs(const ide_registers_t *regs)
{
    fpga_wrcmd(FPGA_CMD_WRITE_IDE_REGS, (const uint8_t*)regs, sizeof(*regs));
}

// Data writes to IDE bus
void ide_phy_start_write(uint32_t blocklen, int udma_mode)
{
    // dbgmsg("ide_phy_start_write(", (int)blocklen, ", ", udma_mode, ")");
    g_ide_phy.crc_errors = 0;
    g_ide_phy.block_crc1 = g_ide_phy.block_crc0 = 0;
    assert((blocklen & 1) == 0);
    uint16_t last_word_idx = blocklen / 2 - 1;
    if (udma_mode < 0)
    {
        fpga_wrcmd(FPGA_CMD_START_WRITE, (const uint8_t*)&last_word_idx, 2);
        g_ide_phy.udma_mode = -1;
    }
    else
    {
        uint8_t arg[3] = {(uint8_t)udma_mode,
                          (uint8_t)(last_word_idx),
                          (uint8_t)((last_word_idx) >> 8)};
        fpga_wrcmd(FPGA_CMD_START_UDMA_WRITE, arg, 3);
        g_ide_phy.udma_mode = udma_mode;
    }
}

bool ide_phy_can_write_block()
{
    uint8_t status;
    fpga_rdcmd(FPGA_CMD_READ_STATUS, &status, 1);
    assert(status & FPGA_STATUS_DATA_DIR);
    return (status & FPGA_STATUS_TX_CANWRITE);
}

void ide_phy_write_block(const uint8_t *buf, uint32_t blocklen)
{
    // dbgmsg("ide_phy_write_block(", bytearray(buf, blocklen), ")");

    if (g_ide_phy.udma_mode >= 0 && (g_ide_phy.block_crc1 & BLOCK_CRC_VALID))
    {
        // Verify the CRC of the previous block before overwriting it
        verify_crc(&g_ide_phy.block_crc1);
    }

    uint16_t crc = 0;
    fpga_wrcmd(FPGA_CMD_WRITE_DATABUF, buf, blocklen, &crc);
    g_ide_phy.transfer_running = true;

    // There can be up to two blocks in FPGA buffers, so store their CRCs separately.
    g_ide_phy.block_crc1 = g_ide_phy.block_crc0;
    g_ide_phy.block_crc0 = crc | BLOCK_CRC_VALID;
}

bool ide_phy_is_write_finished()
{
    uint8_t status;
    fpga_rdcmd(FPGA_CMD_READ_STATUS, &status, 1);
    if (!(status & FPGA_STATUS_DATA_DIR) || (status & FPGA_STATUS_TX_DONE))
    {
        // dbgmsg("ide_phy_is_write_finished() => true");

        if (g_ide_phy.udma_mode >= 0)
        {
            // Verify CRC of last two blocks
            verify_crc(&g_ide_phy.block_crc1);
            verify_crc(&g_ide_phy.block_crc0);
        }

        g_ide_phy.transfer_running = false;
        return true;
    }
    else
    {
        return false;
    }
}

void ide_phy_start_read(uint32_t blocklen, int udma_mode)
{
    // dbgmsg("ide_phy_start_read(", (int)blocklen, ")");
    g_ide_phy.crc_errors = 0;
    g_ide_phy.block_crc1 = g_ide_phy.block_crc0 = 0;
    assert((blocklen & 1) == 0);
    uint16_t last_word_idx = blocklen / 2 - 1;
    fpga_wrcmd(FPGA_CMD_START_READ, (const uint8_t*)&last_word_idx, 2);
    g_ide_phy.transfer_running = true;
    g_ide_phy.udma_mode = -1; // Not supported for read yet

    ide_phy_assert_irq(IDE_STATUS_DEVRDY | IDE_STATUS_DATAREQ);
}

bool ide_phy_can_read_block()
{
    uint8_t status;
    fpga_rdcmd(FPGA_CMD_READ_STATUS, &status, 1);
    assert(!(status & FPGA_STATUS_DATA_DIR));
    return (status & FPGA_STATUS_RX_DONE);
}

void ide_phy_read_block(uint8_t *buf, uint32_t blocklen)
{
    fpga_rdcmd(FPGA_CMD_READ_DATABUF, buf, blocklen);
    // dbgmsg("ide_phy_read_block(", bytearray(buf, blocklen), ")");
}

void ide_phy_stop_transfers(int *crc_errors)
{
    // Configure buffer in write mode but don't write any data => transfer stopped
    uint16_t arg = 65535;
    fpga_wrcmd(FPGA_CMD_START_WRITE, (const uint8_t*)&arg, 2);
    // dbgmsg("ide_phy_stop_transfers()");
    g_ide_phy.transfer_running = false;
    g_ide_phy.udma_mode = -1;

    if (crc_errors) *crc_errors = g_ide_phy.crc_errors;
}

// Assert IDE interrupt and set status register
void ide_phy_assert_irq(uint8_t ide_status)
{
    fpga_wrcmd(FPGA_CMD_ASSERT_IRQ, &ide_status, 1);
    // dbgmsg("ide_phy_assert_irq(", ide_status, ")");
}

const ide_phy_capabilities_t *ide_phy_get_capabilities()
{
    return &g_ide_phy_capabilities;
}
