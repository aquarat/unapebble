/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/boot_trace/boot_trace.h>
#include <pbl/devicetree.h>
#include <pbl/drivers/mmc/stm32_sdmmc.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/idle.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/time.h>
#include <pbl/logging/logging.h>
#include <pbl/soc/st/idle.h>
#include <pbl/util/bits.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>

#include <hardware/stm32_sdmmc.h>

PBL_LOG_MODULE_DECLARE(driver_mmc, CONFIG_DRIVER_MMC_LOG_LEVEL);

// Commands (JESD84-B51, 6.10.4)
#define CMD_GO_IDLE_STATE     0U
#define CMD_SEND_OP_COND      1U
#define CMD_ALL_SEND_CID      2U
#define CMD_SET_RELATIVE_ADDR 3U
#define CMD_SWITCH            6U
#define CMD_SELECT_CARD       7U
#define CMD_SEND_EXT_CSD      8U
#define CMD_STOP_TRANSMISSION 12U
#define CMD_SEND_STATUS       13U
#define CMD_READ_SINGLE_BLOCK 17U
#define CMD_READ_MULTIPLE     18U
#define CMD_SET_BLOCK_COUNT   23U
#define CMD_WRITE_MULTIPLE    25U

// Sector addressing, 1.70-1.95 V (the I/O of the board is at 1.8 V)
#define OCR_ARG         0xC0000080U
#define OCR_BUSY        PBL_BIT(31)
#define OCR_SECTOR_MODE PBL_BIT(30)

// R1 card status: the error bits, READY_FOR_DATA and CURRENT_STATE
#define R1_ERRORS         0xFDF98080U
#define R1_READY_FOR_DATA PBL_BIT(8)
#define R1_STATE_MASK     PBL_GENMASK(12, 9)
#define R1_STATE_TRAN     4U

#define SET_BLOCK_COUNT_RELIABLE PBL_BIT(31)

// The relative address the host gives the card
#define RCA 1U

// EXT_CSD fields
#define EXT_CSD_PARTITION_CONFIG 179U
#define EXT_CSD_BUS_WIDTH        183U
#define EXT_CSD_HS_TIMING        185U
#define EXT_CSD_POWER_CLASS      187U
#define EXT_CSD_CARD_TYPE        196U
#define EXT_CSD_PWR_CL_52_195    200U
#define EXT_CSD_PWR_CL_26_195    201U
#define EXT_CSD_SEC_COUNT        212U
#define EXT_CSD_WR_REL_PARAM     166U
#define EXT_CSD_REL_WR_SEC_C     222U
#define EXT_CSD_GENERIC_CMD6     248U

#define CARD_TYPE_HS_52   PBL_BIT(1)
#define WR_REL_PARAM_EN   PBL_BIT(2)
#define PARTITION_ACCESS  PBL_GENMASK(2, 0)
#define BUS_WIDTH_4       1U
#define SWITCH_WRITE_BYTE 3U

#define INIT_HZ    400000U
#define DEFAULT_HZ 26000000U
#define HS_HZ      52000000U

// Time to stay off before powering up again, for the supply to fall
#define POWER_OFF_MIN_MS 10U

#define CMD_TIMEOUT_US    10000U
#define OCR_TIMEOUT_MS    1000U
#define BUSY_TIMEOUT_MS   3000U
#define SWITCH_DEFAULT_MS 500U
// The data timer: between the blocks of a read, and for the card's busy after a block written.
// The watch's card takes about 130 ms for the first write after it is powered up.
#define READ_TIMEOUT_MS  250U
#define WRITE_TIMEOUT_MS 1000U
// How long a wait spins before it sleeps between polls
#define SPIN_US 200U

// Blocks per command, which keeps a transfer under its timeout
#define MAX_BLOCKS 1024U

#define BLOCK_SIZE_LOG2 9U

static const struct pbl_mmc_stm32_sdmmc *prv_sd(const struct pbl_mmc *mmc) {
  return PBL_CONTAINER_OF(mmc, const struct pbl_mmc_stm32_sdmmc, mmc);
}

static inline uint32_t prv_getreg(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t offset) {
  return pbl_readl(sd->base + offset);
}

static inline void prv_putreg(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t offset,
                              uint32_t value) {
  pbl_writel(value, sd->base + offset);
}

static void prv_delay_ms(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t ms) {
  if (pbl_mmc_in_coredump(&sd->mmc)) {
    pbl_busy_wait_us(ms * 1000U);
  } else {
    pbl_sleep_ms(ms);
  }
}

static uint32_t prv_elapsed_us(uint32_t start) {
  return (pbl_cycle_get_32() - start) / (pbl_cycles_per_sec() / 1000000U);
}

// Between polls of something that takes long: spin first, then sleep, unless in a coredump
static void prv_poll_pause(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t elapsed_us) {
  if (elapsed_us >= SPIN_US && !pbl_mmc_in_coredump(&sd->mmc)) {
    pbl_sleep_ms(1U);
  }
}

static uint32_t prv_clkdiv(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t hz) {
  const uint32_t ker_hz = sd->state->ker_hz;

  // SDMMC_CK is the kernel clock divided by twice CLKDIV, or the kernel clock with 0
  if (hz >= ker_hz) {
    return 0U;
  }

  return PBL_MIN((ker_hz + 2U * hz - 1U) / (2U * hz), (uint32_t)SDMMC_CLKCR_CLKDIV_MASK);
}

static void prv_set_clock(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t hz, uint32_t extra) {
  uint32_t clkcr = prv_getreg(sd, STM32_SDMMC_CLKCR_OFFSET);

  clkcr &= ~(SDMMC_CLKCR_CLKDIV_MASK | SDMMC_CLKCR_BUSSPEED | SDMMC_CLKCR_PWRSAV);
  clkcr |= PBL_FIELD_PREP(SDMMC_CLKCR_CLKDIV_MASK, prv_clkdiv(sd, hz)) | extra;
  prv_putreg(sd, STM32_SDMMC_CLKCR_OFFSET, clkcr);
}

static int prv_cmd(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t idx, uint32_t arg, uint32_t resp,
                   uint32_t flags, uint32_t *r1) {
  const uint32_t done = (resp == SDMMC_CMD_WAITRESP_NONE) ? SDMMC_STA_CMDSENT : SDMMC_STA_CMDREND;
  const uint32_t errors =
      SDMMC_STA_CTIMEOUT | (resp == SDMMC_CMD_WAITRESP_NO_CRC ? 0U : SDMMC_STA_CCRCFAIL);
  const uint32_t start = pbl_cycle_get_32();
  uint32_t sta;

  prv_putreg(sd, STM32_SDMMC_ICR_OFFSET, SDMMC_STA_STATIC_FLAGS & ~SDMMC_STA_DATAEND);
  prv_putreg(sd, STM32_SDMMC_ARG_OFFSET, arg);
  prv_putreg(sd, STM32_SDMMC_CMD_OFFSET,
             PBL_FIELD_PREP(SDMMC_CMD_CMDINDEX_MASK, idx) |
                 PBL_FIELD_PREP(SDMMC_CMD_WAITRESP_MASK, resp) | flags | SDMMC_CMD_CPSMEN);

  for (;;) {
    // The time first, then the status: a thread preempted past the timeout between the two
    // still sees a command that ended meanwhile
    const bool late = prv_elapsed_us(start) > CMD_TIMEOUT_US;

    sta = prv_getreg(sd, STM32_SDMMC_STA_OFFSET);
    if ((sta & (done | errors)) != 0U) {
      break;
    }
    if (late) {
      PBL_LOG_ERR("CMD%" PRIu32 " lost, STA 0x%08" PRIx32, idx, sta);
      PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_SDMMC_STA, (idx << 24) | (sta & 0x00FFFFFFU));
      return -ETIMEDOUT;
    }
  }
  prv_putreg(sd, STM32_SDMMC_ICR_OFFSET, done | errors);

  if ((sta & SDMMC_STA_CTIMEOUT) != 0U) {
    return -ETIMEDOUT;
  }
  if ((sta & SDMMC_STA_CCRCFAIL) != 0U) {
    PBL_LOG_ERR("CMD%" PRIu32 " response CRC", idx);
    return -EIO;
  }

  if (resp == SDMMC_CMD_WAITRESP_SHORT) {
    const uint32_t status = prv_getreg(sd, STM32_SDMMC_RESP1_OFFSET);

    if (r1 != nullptr) {
      *r1 = status;
    }
    if ((status & R1_ERRORS) != 0U) {
      PBL_LOG_ERR("CMD%" PRIu32 " status 0x%08" PRIx32, idx, status);
      return -EIO;
    }
  } else if (r1 != nullptr) {
    *r1 = prv_getreg(sd, STM32_SDMMC_RESP1_OFFSET);
  }

  return 0;
}

// Until the card is back in the transfer state, ready for data, after a busy command or a write
static int prv_wait_ready(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t timeout_ms) {
  const uint32_t start = pbl_cycle_get_32();
  uint32_t r1;
  int ret;

  for (;;) {
    // The time before the status, which then has the last word
    const uint32_t elapsed_us = prv_elapsed_us(start);

    ret = prv_cmd(sd, CMD_SEND_STATUS, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, &r1);
    if (ret != 0) {
      return ret;
    }
    if ((r1 & R1_READY_FOR_DATA) != 0U && PBL_FIELD_GET(R1_STATE_MASK, r1) == R1_STATE_TRAN) {
      return 0;
    }

    if (elapsed_us > timeout_ms * 1000U) {
      PBL_LOG_ERR("Card busy, status 0x%08" PRIx32, r1);
      return -ETIMEDOUT;
    }
    prv_poll_pause(sd, elapsed_us);
  }
}

static int prv_switch(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t index, uint32_t value) {
  const uint32_t arg = (SWITCH_WRITE_BYTE << 24) | (index << 16) | (value << 8);
  const uint32_t cmd6_ms = sd->state->cmd6_time_10ms * 10U;
  int ret;

  ret = prv_cmd(sd, CMD_SWITCH, arg, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  if (ret != 0) {
    return ret;
  }

  // SWITCH_ERROR shows in the status that follows
  ret = prv_wait_ready(sd, cmd6_ms != 0U ? cmd6_ms : SWITCH_DEFAULT_MS);
  if (ret != 0) {
    PBL_LOG_ERR("Switch of EXT_CSD[%" PRIu32 "] to %" PRIu32 " failed: %d", index, value, ret);
  }

  return ret;
}

static void prv_data_stop(const struct pbl_mmc_stm32_sdmmc *sd) {
  pbl_modifyl(SDMMC_CMD_CMDTRANS, 0U, sd->base + STM32_SDMMC_CMD_OFFSET);
  prv_putreg(sd, STM32_SDMMC_DCTRL_OFFSET, 0U);
  prv_putreg(sd, STM32_SDMMC_IDMACTRL_OFFSET, 0U);
  prv_putreg(sd, STM32_SDMMC_ICR_OFFSET, SDMMC_STA_STATIC_FLAGS);
}

// One data command over the internal DMA, @p buf word aligned in SRAM
static int prv_data(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t idx, uint32_t arg, void *buf,
                    uint32_t count, bool read) {
  const uint32_t errors = SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT | SDMMC_STA_RXOVERR |
                          SDMMC_STA_TXUNDERR | SDMMC_STA_IDMATE;
  const uint32_t data_ms = read ? READ_TIMEOUT_MS : WRITE_TIMEOUT_MS;
  // Past the data timer, which reports the error itself, and the transfer of the blocks
  const uint32_t timeout_us = (data_ms + 100U + count * 2U) * 1000U;
  const uint32_t div =
      PBL_FIELD_GET(SDMMC_CLKCR_CLKDIV_MASK, prv_getreg(sd, STM32_SDMMC_CLKCR_OFFSET));
  const uint32_t ck_hz = (div == 0U) ? sd->state->ker_hz : sd->state->ker_hz / (2U * div);
  uint32_t start;
  uint32_t sta;
  int ret;

  prv_putreg(sd, STM32_SDMMC_ICR_OFFSET, SDMMC_STA_STATIC_FLAGS);
  prv_putreg(sd, STM32_SDMMC_DTIMER_OFFSET, (ck_hz / 1000U) * data_ms);
  prv_putreg(sd, STM32_SDMMC_DLEN_OFFSET, count * PBL_MMC_BLOCK_SIZE);
  prv_putreg(sd, STM32_SDMMC_IDMABASER_OFFSET, (uint32_t)(uintptr_t)buf);
  prv_putreg(sd, STM32_SDMMC_IDMACTRL_OFFSET, SDMMC_IDMACTRL_IDMAEN);
  // The command starts the transfer, of whole blocks
  prv_putreg(sd, STM32_SDMMC_DCTRL_OFFSET,
             PBL_FIELD_PREP(SDMMC_DCTRL_DBLOCKSIZE_MASK, BLOCK_SIZE_LOG2) |
                 (read ? SDMMC_DCTRL_DTDIR : 0U));

  ret = prv_cmd(sd, idx, arg, SDMMC_CMD_WAITRESP_SHORT, SDMMC_CMD_CMDTRANS, nullptr);
  if (ret == 0) {
    start = pbl_cycle_get_32();
    for (;;) {
      // As for a command: the time before the status
      const uint32_t elapsed_us = prv_elapsed_us(start);

      sta = prv_getreg(sd, STM32_SDMMC_STA_OFFSET);
      if ((sta & (SDMMC_STA_DATAEND | errors)) != 0U) {
        break;
      }
      if (elapsed_us > timeout_us) {
        sta |= SDMMC_STA_DTIMEOUT;
        break;
      }
      prv_poll_pause(sd, elapsed_us);
    }

    if ((sta & errors) != 0U) {
      PBL_LOG_ERR("CMD%" PRIu32 " data error, STA 0x%08" PRIx32, idx, sta);
      PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_SDMMC_STA, (idx << 24) | (sta & 0x00FFFFFFU));
      ret = (sta & (SDMMC_STA_DTIMEOUT)) != 0U ? -ETIMEDOUT : -EIO;
    }
  }

  if (ret != 0) {
    // Abort what the card still runs, then let it settle back in the transfer state
    prv_putreg(sd, STM32_SDMMC_DCTRL_OFFSET, SDMMC_DCTRL_FIFORST);
    (void)prv_cmd(sd, CMD_STOP_TRANSMISSION, 0U, SDMMC_CMD_WAITRESP_SHORT, SDMMC_CMD_CMDSTOP,
                  nullptr);
    pbl_modifyl(SDMMC_CMD_CMDSTOP, 0U, sd->base + STM32_SDMMC_CMD_OFFSET);
  }
  prv_data_stop(sd);

  return ret;
}

static int prv_init(const struct pbl_mmc *mmc) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd(mmc);
  uint64_t rate_mhz;
  int ret;

  ret = pbl_device_init(&sd->enable.port->dev);
  if (ret == 0) {
    ret = pbl_gpio_configure(&sd->enable, PBL_GPIO_OUTPUT_INACTIVE);
  }
  if (ret != 0) {
    return ret;
  }
  sd->state->off_since_ms = 0U;

  ret = pbl_pinctrl_select_state(&sd->pinctrl, PBL_PINCTRL_STATE_SLEEP);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_device_init(&sd->clk.ctrl->dev);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_clk_get_rate(&sd->clk, &rate_mhz);
  if (ret != 0) {
    return ret;
  }
  sd->state->ker_hz = (uint32_t)(rate_mhz / 1000U);
  if (sd->state->ker_hz < 2U * INIT_HZ) {
    PBL_LOG_ERR("Kernel clock of %" PRIu32 " Hz", sd->state->ker_hz);
    return -EINVAL;
  }

  ret = pbl_device_init(&sd->rst.ctrl->dev);
  if (ret == 0) {
    ret = pbl_reset_reset(&sd->rst);
  }

  return ret;
}

static void prv_power_off(const struct pbl_mmc *mmc) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd(mmc);

  if (pbl_clk_is_enabled(&sd->clk)) {
    prv_putreg(sd, STM32_SDMMC_POWER_OFFSET, SDMMC_POWER_PWRCTRL_OFF);
    (void)pbl_clk_disable(&sd->clk);
  }
  (void)pbl_pinctrl_select_state(&sd->pinctrl, PBL_PINCTRL_STATE_SLEEP);
  (void)pbl_gpio_set(&sd->enable, false);
  if (!pbl_mmc_in_coredump(mmc)) {
    sd->state->off_since_ms = pbl_uptime_ms();
  }
}

// CMD0, then CMD1 until the card is out of its power-up, CMD2 and CMD3, and CMD7 to select it
static int prv_identify(const struct pbl_mmc_stm32_sdmmc *sd) {
  const uint32_t start = pbl_cycle_get_32();
  uint32_t ocr;
  int ret;

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_CMD0);
  ret = prv_cmd(sd, CMD_GO_IDLE_STATE, 0U, SDMMC_CMD_WAITRESP_NONE, 0U, nullptr);
  if (ret != 0) {
    return ret;
  }

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_CMD1);
  for (;;) {
    ret = prv_cmd(sd, CMD_SEND_OP_COND, OCR_ARG, SDMMC_CMD_WAITRESP_NO_CRC, 0U, &ocr);
    if (ret != 0) {
      return ret;
    }
    if ((ocr & OCR_BUSY) != 0U) {
      break;
    }
    if (prv_elapsed_us(start) > OCR_TIMEOUT_MS * 1000U) {
      PBL_LOG_ERR("Card stays busy, OCR 0x%08" PRIx32, ocr);
      return -ETIMEDOUT;
    }
    prv_delay_ms(sd, 1U);
  }

  PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_MMC_OCR, ocr);
  if ((ocr & OCR_SECTOR_MODE) == 0U) {
    PBL_LOG_ERR("Card addressed in bytes, OCR 0x%08" PRIx32, ocr);
    return -ENOTSUP;
  }

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_CID_RCA_SELECT);
  ret = prv_cmd(sd, CMD_ALL_SEND_CID, 0U, SDMMC_CMD_WAITRESP_LONG, 0U, nullptr);
  if (ret == 0) {
    ret = prv_cmd(sd, CMD_SET_RELATIVE_ADDR, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  }
  if (ret == 0) {
    ret = prv_cmd(sd, CMD_SELECT_CARD, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  }
  if (ret == 0) {
    ret = prv_wait_ready(sd, BUSY_TIMEOUT_MS);
  }

  return ret;
}

static uint32_t prv_sec_count(const uint8_t *ext_csd) {
  return (uint32_t)ext_csd[EXT_CSD_SEC_COUNT] | ((uint32_t)ext_csd[EXT_CSD_SEC_COUNT + 1U] << 8) |
         ((uint32_t)ext_csd[EXT_CSD_SEC_COUNT + 2U] << 16) |
         ((uint32_t)ext_csd[EXT_CSD_SEC_COUNT + 3U] << 24);
}

// The user area, the power class of the bus, four data lines, then High Speed if the card has it
static int prv_configure(const struct pbl_mmc_stm32_sdmmc *sd, uint32_t *num_blocks) {
  struct pbl_mmc_stm32_sdmmc_state *st = sd->state;
  const uint8_t *ext_csd = st->buf;
  bool hs;
  uint32_t pwr_cl;
  int ret;

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_EXT_CSD);
  ret = prv_data(sd, CMD_SEND_EXT_CSD, 0U, st->buf, 1U, true);
  if (ret != 0) {
    return ret;
  }

  *num_blocks = prv_sec_count(ext_csd);
  st->cmd6_time_10ms = ext_csd[EXT_CSD_GENERIC_CMD6];
  st->reliable = (ext_csd[EXT_CSD_WR_REL_PARAM] & WR_REL_PARAM_EN) != 0U;
  st->rel_sectors = ext_csd[EXT_CSD_REL_WR_SEC_C];
  hs =
      !st->no_hs && (ext_csd[EXT_CSD_CARD_TYPE] & CARD_TYPE_HS_52) != 0U && sd->max_hz > DEFAULT_HZ;
  pwr_cl = (hs ? ext_csd[EXT_CSD_PWR_CL_52_195] : ext_csd[EXT_CSD_PWR_CL_26_195]) & 0x0FU;

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_PARTITION);
  if ((ext_csd[EXT_CSD_PARTITION_CONFIG] & PARTITION_ACCESS) != 0U) {
    ret = prv_switch(sd, EXT_CSD_PARTITION_CONFIG,
                     ext_csd[EXT_CSD_PARTITION_CONFIG] & ~PARTITION_ACCESS);
    if (ret != 0) {
      return ret;
    }
  }

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_POWER_CLASS);
  if (pwr_cl != (ext_csd[EXT_CSD_POWER_CLASS] & 0x0FU)) {
    ret = prv_switch(sd, EXT_CSD_POWER_CLASS, pwr_cl);
    if (ret != 0) {
      return ret;
    }
  }

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_BUS_WIDTH);
  ret = prv_switch(sd, EXT_CSD_BUS_WIDTH, BUS_WIDTH_4);
  if (ret != 0) {
    return ret;
  }
  pbl_modifyl(SDMMC_CLKCR_WIDBUS_MASK,
              PBL_FIELD_PREP(SDMMC_CLKCR_WIDBUS_MASK, SDMMC_CLKCR_WIDBUS_4),
              sd->base + STM32_SDMMC_CLKCR_OFFSET);

  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_HS | (hs ? 0x100U : 0U));
  if (hs) {
    ret = prv_switch(sd, EXT_CSD_HS_TIMING, 1U);
    if (ret != 0) {
      return ret;
    }
    prv_set_clock(sd, PBL_MIN(sd->max_hz, HS_HZ), SDMMC_CLKCR_BUSSPEED | SDMMC_CLKCR_PWRSAV);
  } else {
    prv_set_clock(sd, PBL_MIN(sd->max_hz, DEFAULT_HZ), SDMMC_CLKCR_PWRSAV);
  }

  // Read the EXT_CSD again on the bus as it now runs: a bus that does not, fails here
  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_EXT_CSD_AGAIN);
  ret = prv_data(sd, CMD_SEND_EXT_CSD, 0U, st->buf, 1U, true);
  if (ret == 0 && prv_sec_count(ext_csd) != *num_blocks) {
    ret = -EIO;
  }
  if (ret != 0 && hs) {
    PBL_LOG_WRN("High Speed bus fails, falling back to %u Hz", DEFAULT_HZ);
    st->no_hs = true;
  }

  return ret;
}

static int prv_power_on(const struct pbl_mmc *mmc, uint32_t *num_blocks) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd(mmc);
  struct pbl_mmc_stm32_sdmmc_state *st = sd->state;
  int ret;

  pbl_idle_state_block(SOC_STM32_IDLE_STOP);
  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_SUPPLY);

  // A supply that just went off has to fall first
  if (pbl_mmc_in_coredump(mmc)) {
    prv_delay_ms(sd, POWER_OFF_MIN_MS);
  } else {
    const uint64_t off_ms = pbl_uptime_ms() - st->off_since_ms;

    if (off_ms < POWER_OFF_MIN_MS) {
      prv_delay_ms(sd, POWER_OFF_MIN_MS - (uint32_t)off_ms);
    }
  }

  ret = pbl_clk_enable(&sd->clk);
  if (ret == 0) {
    ret = pbl_pinctrl_select_state(&sd->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
  }
  if (ret == 0) {
    ret = pbl_gpio_set(&sd->enable, true);
  }
  if (ret != 0) {
    prv_power_off(mmc);
    pbl_idle_state_release(SOC_STM32_IDLE_STOP);
    return ret;
  }
  prv_delay_ms(sd, sd->power_on_delay_ms);

  // One data line at 400 kHz, the clock running, then the 74 clocks the card needs
  PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_CLOCK);
  prv_putreg(sd, STM32_SDMMC_POWER_OFFSET, SDMMC_POWER_PWRCTRL_OFF);
  prv_putreg(
      sd, STM32_SDMMC_CLKCR_OFFSET,
      PBL_FIELD_PREP(SDMMC_CLKCR_CLKDIV_MASK, prv_clkdiv(sd, INIT_HZ)) | SDMMC_CLKCR_HWFC_EN);
  prv_putreg(sd, STM32_SDMMC_POWER_OFFSET, SDMMC_POWER_PWRCTRL_ON);
  prv_delay_ms(sd, 1U);

  ret = prv_identify(sd);
  if (ret == 0) {
    ret = prv_configure(sd, num_blocks);
  }
  if (ret != 0) {
    PBL_LOG_ERR("Card init failed: %d", ret);
    PBL_BOOT_TRACE_NOTE(PBL_BT_NOTE_MMC_ERR, (uint32_t)ret);
    PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_FAILED);
    prv_power_off(mmc);
  } else {
    PBL_BOOT_TRACE(PBL_BT_SDMMC, PBL_BT_SDMMC_READY);
  }

  pbl_idle_state_release(SOC_STM32_IDLE_STOP);

  return ret;
}

// SRAM the internal DMA reaches, the buffer word aligned
static bool prv_dma_ok(const void *buf) {
  const uintptr_t addr = (uintptr_t)buf;

  return (addr & 3U) == 0U && addr >= 0x20000000U && addr < 0x28000000U;
}

static int prv_read(const struct pbl_mmc *mmc, uint32_t lba, void *buf, uint32_t count) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd(mmc);
  uint8_t *dst = buf;
  int ret = 0;

  pbl_idle_state_block(SOC_STM32_IDLE_STOP);
  while (count > 0U && ret == 0) {
    if (!prv_dma_ok(dst)) {
      ret = prv_data(sd, CMD_READ_SINGLE_BLOCK, lba, sd->state->buf, 1U, true);
      if (ret == 0) {
        memcpy(dst, sd->state->buf, PBL_MMC_BLOCK_SIZE);
      }
      lba++;
      dst += PBL_MMC_BLOCK_SIZE;
      count--;
      continue;
    }

    const uint32_t n = PBL_MIN(count, MAX_BLOCKS);
    if (n == 1U) {
      ret = prv_data(sd, CMD_READ_SINGLE_BLOCK, lba, dst, 1U, true);
    } else {
      ret = prv_cmd(sd, CMD_SET_BLOCK_COUNT, n, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
      if (ret == 0) {
        ret = prv_data(sd, CMD_READ_MULTIPLE, lba, dst, n, true);
      }
    }
    lba += n;
    dst += n * PBL_MMC_BLOCK_SIZE;
    count -= n;
  }
  pbl_idle_state_release(SOC_STM32_IDLE_STOP);

  return ret;
}

// Blocks of a reliable write: any number with the enhanced definition, else whole groups of
// REL_WR_SEC_C, or single blocks
static uint32_t prv_write_chunk(const struct pbl_mmc_stm32_sdmmc_state *st, uint32_t lba,
                                uint32_t count) {
  if (st->reliable) {
    return PBL_MIN(count, MAX_BLOCKS);
  }
  if (st->rel_sectors > 1U && (lba % st->rel_sectors) == 0U && count >= st->rel_sectors) {
    return st->rel_sectors;
  }

  return 1U;
}

static int prv_write(const struct pbl_mmc *mmc, uint32_t lba, const void *buf, uint32_t count) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd(mmc);
  struct pbl_mmc_stm32_sdmmc_state *st = sd->state;
  const uint8_t *src = buf;
  int ret = 0;

  pbl_idle_state_block(SOC_STM32_IDLE_STOP);
  while (count > 0U && ret == 0) {
    const bool direct = prv_dma_ok(src);
    const uint32_t n = direct ? prv_write_chunk(st, lba, count) : 1U;
    void *data = (void *)src;

    if (!direct) {
      memcpy(st->buf, src, PBL_MMC_BLOCK_SIZE);
      data = st->buf;
    }

    ret = prv_cmd(sd, CMD_SET_BLOCK_COUNT, n | SET_BLOCK_COUNT_RELIABLE, SDMMC_CMD_WAITRESP_SHORT,
                  0U, nullptr);
    if (ret == 0) {
      ret = prv_data(sd, CMD_WRITE_MULTIPLE, lba, data, n, false);
    }
    if (ret == 0) {
      ret = prv_wait_ready(sd, BUSY_TIMEOUT_MS);
    }
    lba += n;
    src += n * PBL_MMC_BLOCK_SIZE;
    count -= n;
  }
  pbl_idle_state_release(SOC_STM32_IDLE_STOP);

  return ret;
}

const struct pbl_mmc_ops pbl_mmc_stm32_sdmmc_ops = {
  .init = prv_init,
  .power_on = prv_power_on,
  .power_off = prv_power_off,
  .read = prv_read,
  .write = prv_write,
};

#include PBL_DT_INSTANCES(st_stm32_sdmmc2)
