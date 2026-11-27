/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The eMMC step by step, on the SDMMC registers, as the driver brings it up, each command's
// response on screen. Writes only to the Lab's scratch blocks in the GUI region.

#include "lab.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/crc/crc.h>
#include <pbl/drivers/clk.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/mmc.h>
#include <pbl/drivers/mmc/stm32_sdmmc.h>
#include <pbl/drivers/pinctrl.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/time.h>
#include <pbl/util/bits.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#include <hardware/stm32_sdmmc.h>

#define RCA 1U

#define R1_ERRORS         0xFDF98080U
#define R1_READY_FOR_DATA PBL_BIT(8)
#define R1_STATE(r1)      (((r1) >> 9) & 0xFU)
#define R1_STATE_TRAN     4U

#define OCR_ARG 0xC0000080U

#define EXT_CSD_PARTITION_CONFIG 179U
#define EXT_CSD_BUS_WIDTH        183U
#define EXT_CSD_HS_TIMING        185U
#define EXT_CSD_POWER_CLASS      187U
#define EXT_CSD_REV              192U
#define EXT_CSD_CARD_TYPE        196U
#define EXT_CSD_PWR_CL_52_195    200U
#define EXT_CSD_PWR_CL_26_195    201U
#define EXT_CSD_SEC_COUNT        212U
#define EXT_CSD_BOOT_SIZE_MULT   226U
#define EXT_CSD_RPMB_SIZE_MULT   168U
#define EXT_CSD_WR_REL_PARAM     166U
#define EXT_CSD_WR_REL_SET       167U
#define EXT_CSD_REL_WR_SEC_C     222U
#define EXT_CSD_GENERIC_CMD6     248U

#define INIT_HZ    400000U
#define DEFAULT_HZ 26000000U
#define HS_HZ      52000000U

#define CMD_TIMEOUT_US 10000U
#define BUSY_MS        3000U
#define DATA_MS        250U

#define MULTI_BLOCKS 64U

static struct {
  uint32_t ker_hz;
  uint32_t cmd6_ms;
  uint32_t sec_count;
  bool hs_card;
  bool on;
  uint8_t *ext;
  uint8_t *buf;
} s;

static const struct pbl_mmc_stm32_sdmmc *prv_sd(void) {
  return PBL_CONTAINER_OF(MMC, const struct pbl_mmc_stm32_sdmmc, mmc);
}

static uint32_t prv_get(uint32_t off) {
  return pbl_readl(prv_sd()->base + off);
}

static void prv_put(uint32_t off, uint32_t v) {
  pbl_writel(v, prv_sd()->base + off);
}

static uint32_t prv_us(uint32_t start) {
  return (pbl_cycle_get_32() - start) / (pbl_cycles_per_sec() / 1000000U);
}

static uint32_t prv_clkdiv(uint32_t hz) {
  if (hz >= s.ker_hz) {
    return 0U;
  }

  return PBL_MIN((s.ker_hz + 2U * hz - 1U) / (2U * hz), (uint32_t)SDMMC_CLKCR_CLKDIV_MASK);
}

static uint32_t prv_bus_hz(void) {
  const uint32_t div = PBL_FIELD_GET(SDMMC_CLKCR_CLKDIV_MASK, prv_get(STM32_SDMMC_CLKCR_OFFSET));

  return (div == 0U) ? s.ker_hz : s.ker_hz / (2U * div);
}

static void prv_set_clock(uint32_t hz, uint32_t extra) {
  uint32_t clkcr = prv_get(STM32_SDMMC_CLKCR_OFFSET);

  clkcr &= ~(SDMMC_CLKCR_CLKDIV_MASK | SDMMC_CLKCR_BUSSPEED | SDMMC_CLKCR_PWRSAV);
  clkcr |= PBL_FIELD_PREP(SDMMC_CLKCR_CLKDIV_MASK, prv_clkdiv(hz)) | extra;
  prv_put(STM32_SDMMC_CLKCR_OFFSET, clkcr);
}

// A command; resp gets RESP1 to RESP4
static int prv_cmd(uint32_t idx, uint32_t arg, uint32_t wait, uint32_t flags, uint32_t resp[4]) {
  const uint32_t done = (wait == SDMMC_CMD_WAITRESP_NONE) ? SDMMC_STA_CMDSENT : SDMMC_STA_CMDREND;
  const uint32_t errors =
      SDMMC_STA_CTIMEOUT | (wait == SDMMC_CMD_WAITRESP_NO_CRC ? 0U : SDMMC_STA_CCRCFAIL);
  const uint32_t start = pbl_cycle_get_32();
  uint32_t sta;

  prv_put(STM32_SDMMC_ICR_OFFSET, SDMMC_STA_STATIC_FLAGS & ~SDMMC_STA_DATAEND);
  prv_put(STM32_SDMMC_ARG_OFFSET, arg);
  prv_put(STM32_SDMMC_CMD_OFFSET, PBL_FIELD_PREP(SDMMC_CMD_CMDINDEX_MASK, idx) |
                                      PBL_FIELD_PREP(SDMMC_CMD_WAITRESP_MASK, wait) | flags |
                                      SDMMC_CMD_CPSMEN);
  for (;;) {
    sta = prv_get(STM32_SDMMC_STA_OFFSET);
    if ((sta & (done | errors)) != 0U) {
      break;
    }
    if (prv_us(start) > CMD_TIMEOUT_US) {
      lab_printf("CMD%lu: no end, STA 0x%08" PRIx32, (unsigned long)idx, sta);
      return -ETIMEDOUT;
    }
  }
  prv_put(STM32_SDMMC_ICR_OFFSET, done | errors);

  if (resp != nullptr) {
    resp[0] = prv_get(STM32_SDMMC_RESP1_OFFSET);
    resp[1] = prv_get(STM32_SDMMC_RESP2_OFFSET);
    resp[2] = prv_get(STM32_SDMMC_RESP3_OFFSET);
    resp[3] = prv_get(STM32_SDMMC_RESP4_OFFSET);
  }
  if ((sta & SDMMC_STA_CTIMEOUT) != 0U) {
    lab_printf("CMD%lu: timeout", (unsigned long)idx);
    return -ETIMEDOUT;
  }
  if ((sta & SDMMC_STA_CCRCFAIL) != 0U) {
    lab_printf("CMD%lu: CRC", (unsigned long)idx);
    return -EIO;
  }
  if (wait == SDMMC_CMD_WAITRESP_SHORT) {
    const uint32_t r1 = prv_get(STM32_SDMMC_RESP1_OFFSET);

    if ((r1 & R1_ERRORS) != 0U) {
      lab_printf("CMD%lu: R1 0x%08" PRIx32, (unsigned long)idx, r1);
      return -EIO;
    }
  }

  return 0;
}

static int prv_wait_ready(uint32_t timeout_ms, uint32_t *polls) {
  const uint32_t start = pbl_cycle_get_32();
  uint32_t r[4];
  int ret;

  *polls = 0U;
  for (;;) {
    ret = prv_cmd(13U, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, r);
    (*polls)++;
    if (ret != 0) {
      return ret;
    }
    if ((r[0] & R1_READY_FOR_DATA) != 0U && R1_STATE(r[0]) == R1_STATE_TRAN) {
      return 0;
    }
    if (prv_us(start) > timeout_ms * 1000U) {
      lab_printf("busy: R1 0x%08" PRIx32, r[0]);
      return -ETIMEDOUT;
    }
    if (prv_us(start) > 200U) {
      pbl_sleep_ms(1U);
    }
  }
}

static int prv_switch(uint32_t index, uint32_t value) {
  uint32_t polls;
  int ret;

  ret =
      prv_cmd(6U, (3U << 24) | (index << 16) | (value << 8), SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  if (ret == 0) {
    ret = prv_wait_ready(s.cmd6_ms != 0U ? s.cmd6_ms : 500U, &polls);
  }
  if (ret != 0) {
    lab_printf("CMD6 [%lu]=%lu: %d", (unsigned long)index, (unsigned long)value, ret);
  }

  return ret;
}

static void prv_data_stop(void) {
  pbl_modifyl(SDMMC_CMD_CMDTRANS, 0U, prv_sd()->base + STM32_SDMMC_CMD_OFFSET);
  prv_put(STM32_SDMMC_DCTRL_OFFSET, 0U);
  prv_put(STM32_SDMMC_IDMACTRL_OFFSET, 0U);
  prv_put(STM32_SDMMC_ICR_OFFSET, SDMMC_STA_STATIC_FLAGS);
}

static int prv_data(uint32_t idx, uint32_t arg, void *buf, uint32_t count, bool read) {
  const uint32_t errors = SDMMC_STA_DCRCFAIL | SDMMC_STA_DTIMEOUT | SDMMC_STA_RXOVERR |
                          SDMMC_STA_TXUNDERR | SDMMC_STA_IDMATE;
  const uint32_t timeout_us = (100U + count * 2U) * 1000U;
  uint32_t start;
  uint32_t sta = 0U;
  int ret;

  prv_put(STM32_SDMMC_ICR_OFFSET, SDMMC_STA_STATIC_FLAGS);
  prv_put(STM32_SDMMC_DTIMER_OFFSET, (prv_bus_hz() / 1000U) * DATA_MS);
  prv_put(STM32_SDMMC_DLEN_OFFSET, count * 512U);
  prv_put(STM32_SDMMC_IDMABASER_OFFSET, (uint32_t)(uintptr_t)buf);
  prv_put(STM32_SDMMC_IDMACTRL_OFFSET, SDMMC_IDMACTRL_IDMAEN);
  prv_put(STM32_SDMMC_DCTRL_OFFSET,
          PBL_FIELD_PREP(SDMMC_DCTRL_DBLOCKSIZE_MASK, 9U) | (read ? SDMMC_DCTRL_DTDIR : 0U));

  ret = prv_cmd(idx, arg, SDMMC_CMD_WAITRESP_SHORT, SDMMC_CMD_CMDTRANS, nullptr);
  if (ret == 0) {
    start = pbl_cycle_get_32();
    for (;;) {
      sta = prv_get(STM32_SDMMC_STA_OFFSET);
      if ((sta & (SDMMC_STA_DATAEND | errors)) != 0U) {
        break;
      }
      if (prv_us(start) > timeout_us) {
        sta |= SDMMC_STA_DTIMEOUT;
        break;
      }
      if (prv_us(start) > 200U) {
        pbl_sleep_ms(1U);
      }
    }
    if ((sta & errors) != 0U) {
      lab_printf("CMD%lu data: STA 0x%08" PRIx32, (unsigned long)idx, sta);
      ret = (sta & SDMMC_STA_DTIMEOUT) != 0U ? -ETIMEDOUT : -EIO;
    }
  }
  if (ret != 0) {
    prv_put(STM32_SDMMC_DCTRL_OFFSET, SDMMC_DCTRL_FIFORST);
    (void)prv_cmd(12U, 0U, SDMMC_CMD_WAITRESP_SHORT, SDMMC_CMD_CMDSTOP, nullptr);
    pbl_modifyl(SDMMC_CMD_CMDSTOP, 0U, prv_sd()->base + STM32_SDMMC_CMD_OFFSET);
  }
  prv_data_stop();

  return ret;
}

static void prv_off(void) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd();

  if (!s.on) {
    return;
  }
  prv_put(STM32_SDMMC_POWER_OFFSET, SDMMC_POWER_PWRCTRL_OFF);
  (void)pbl_clk_disable(&sd->clk);
  (void)pbl_pinctrl_select_state(&sd->pinctrl, PBL_PINCTRL_STATE_SLEEP);
  (void)pbl_gpio_set(&sd->enable, false);
  s.on = false;
}

// Steps

static int prv_clock(void) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd();
  uint64_t rate;
  int ret;

  memset(&s, 0, sizeof(s));
  s.ext = lab_alloc(512U);
  s.buf = lab_alloc(MULTI_BLOCKS * 512U);
  if (s.ext == nullptr || s.buf == nullptr) {
    return -ENOMEM;
  }
  lab_printf("buffers at %p, %p", (void *)s.ext, (void *)s.buf);

  // The driver's card off, and its host reset, with the Lab's choice of clock
  ret = lab_emmc_clock_select(lab_emmc_clock_is_pll1_q());
  if (ret != 0) {
    return ret;
  }
  ret = pbl_clk_get_rate(&sd->clk, &rate);
  if (ret != 0) {
    return ret;
  }
  s.ker_hz = (uint32_t)(rate / 1000U);
  lab_printf("kernel clock %lu Hz (%s)", (unsigned long)s.ker_hz,
             lab_emmc_clock_is_pll1_q() ? "PLL1 Q" : "HSI48");

  return s.ker_hz >= 2U * INIT_HZ ? 0 : -EINVAL;
}

static int prv_supply(void) {
  const struct pbl_mmc_stm32_sdmmc *sd = prv_sd();
  int ret;

  ret = pbl_clk_enable(&sd->clk);
  if (ret == 0) {
    s.on = true;
    ret = pbl_pinctrl_select_state(&sd->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
  }
  if (ret == 0) {
    ret = pbl_gpio_set(&sd->enable, true);
  }
  if (ret == 0) {
    pbl_sleep_ms(sd->power_on_delay_ms);
    lab_printf("EMMC_EN on, waited %lu ms", (unsigned long)sd->power_on_delay_ms);
  }

  return ret;
}

static int prv_host(void) {
  prv_put(STM32_SDMMC_POWER_OFFSET, SDMMC_POWER_PWRCTRL_OFF);
  prv_put(STM32_SDMMC_CLKCR_OFFSET,
          PBL_FIELD_PREP(SDMMC_CLKCR_CLKDIV_MASK, prv_clkdiv(INIT_HZ)) | SDMMC_CLKCR_HWFC_EN);
  prv_put(STM32_SDMMC_POWER_OFFSET, SDMMC_POWER_PWRCTRL_ON);
  pbl_sleep_ms(1U);
  lab_printf("bus %lu Hz, POWER 0x%lx", (unsigned long)prv_bus_hz(),
             (unsigned long)prv_get(STM32_SDMMC_POWER_OFFSET));

  return 0;
}

static int prv_cmd0(void) {
  return prv_cmd(0U, 0U, SDMMC_CMD_WAITRESP_NONE, 0U, nullptr);
}

static int prv_cmd1(void) {
  const uint32_t start = pbl_cycle_get_32();
  uint32_t r[4];
  uint32_t tries = 0U;
  int ret;

  for (;;) {
    ret = prv_cmd(1U, OCR_ARG, SDMMC_CMD_WAITRESP_NO_CRC, 0U, r);
    tries++;
    if (ret != 0) {
      return ret;
    }
    if ((r[0] & PBL_BIT(31)) != 0U) {
      break;
    }
    if (prv_us(start) > 1000000U) {
      lab_printf("still busy, OCR 0x%08" PRIx32, r[0]);
      return -ETIMEDOUT;
    }
    pbl_sleep_ms(1U);
  }
  lab_printf("OCR 0x%08" PRIx32, r[0]);
  lab_printf("%lu tries, %lu ms", (unsigned long)tries, (unsigned long)(prv_us(start) / 1000U));
  lab_printf("%s, %s%s", (r[0] & PBL_BIT(30)) ? "sectors" : "BYTES",
             (r[0] & PBL_BIT(7)) ? "1.8V " : "", (r[0] & 0x00FF8000U) ? "2.7-3.6V" : "");

  return (r[0] & PBL_BIT(30)) ? 0 : -ENOTSUP;
}

static int prv_cmd2(void) {
  uint32_t r[4];
  char pnm[7];
  int ret;

  ret = prv_cmd(2U, 0U, SDMMC_CMD_WAITRESP_LONG, 0U, r);
  if (ret != 0) {
    return ret;
  }
  // CID[127:96] in RESP1: MID, CBX, OID, PNM[47:40]
  pnm[0] = (char)(r[0] & 0xFFU);
  pnm[1] = (char)(r[1] >> 24);
  pnm[2] = (char)(r[1] >> 16);
  pnm[3] = (char)(r[1] >> 8);
  pnm[4] = (char)r[1];
  pnm[5] = (char)(r[2] >> 24);
  pnm[6] = '\0';
  for (size_t i = 0U; i < 6U; i++) {
    if (pnm[i] < ' ' || pnm[i] > '~') {
      pnm[i] = '?';
    }
  }
  lab_printf("MID 0x%02lx OID 0x%02lx", (unsigned long)(r[0] >> 24),
             (unsigned long)((r[0] >> 8) & 0xFFU));
  lab_printf("name %s rev 0x%02lx", pnm, (unsigned long)((r[2] >> 16) & 0xFFU));
  lab_printf("serial 0x%08lx", (unsigned long)(((r[2] & 0xFFFFU) << 16) | (r[3] >> 16)));
  lab_printf("MDT 0x%02lx", (unsigned long)((r[3] >> 8) & 0xFFU));

  return 0;
}

static int prv_cmd3(void) {
  return prv_cmd(3U, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
}

static int prv_cmd9(void) {
  uint32_t r[4];
  int ret;

  ret = prv_cmd(9U, RCA << 16, SDMMC_CMD_WAITRESP_LONG, 0U, r);
  if (ret != 0) {
    return ret;
  }
  lab_printf("CSD %08" PRIx32 " %08" PRIx32, r[0], r[1]);
  lab_printf("    %08" PRIx32 " %08" PRIx32, r[2], r[3]);
  lab_printf("struct %lu spec %lu TRAN_SPEED 0x%02lx", (unsigned long)(r[0] >> 30),
             (unsigned long)((r[0] >> 26) & 0xFU), (unsigned long)(r[0] & 0xFFU));

  return 0;
}

static int prv_cmd7(void) {
  uint32_t r[4];
  uint32_t polls;
  int ret;

  ret = prv_cmd(7U, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  if (ret == 0) {
    ret = prv_wait_ready(BUSY_MS, &polls);
  }
  if (ret == 0) {
    ret = prv_cmd(13U, RCA << 16, SDMMC_CMD_WAITRESP_SHORT, 0U, r);
    lab_printf("status 0x%08" PRIx32 ", tran", r[0]);
  }

  return ret;
}

static uint32_t prv_sec_count(const uint8_t *e) {
  return (uint32_t)e[EXT_CSD_SEC_COUNT] | ((uint32_t)e[EXT_CSD_SEC_COUNT + 1U] << 8) |
         ((uint32_t)e[EXT_CSD_SEC_COUNT + 2U] << 16) | ((uint32_t)e[EXT_CSD_SEC_COUNT + 3U] << 24);
}

static int prv_ext_csd(void) {
  const uint8_t *e = s.ext;
  int ret;

  ret = prv_data(8U, 0U, s.ext, 1U, true);
  if (ret != 0) {
    return ret;
  }
  s.sec_count = prv_sec_count(e);
  s.cmd6_ms = e[EXT_CSD_GENERIC_CMD6] * 10U;
  s.hs_card = (e[EXT_CSD_CARD_TYPE] & PBL_BIT(1)) != 0U;
  lab_printf("rev %u, %lu blocks", e[EXT_CSD_REV], (unsigned long)s.sec_count);
  lab_printf("= %lu MiB", (unsigned long)(s.sec_count / 2048U));
  lab_printf(
      "CARD_TYPE 0x%02x: %s%s%s%s", e[EXT_CSD_CARD_TYPE],
      (e[EXT_CSD_CARD_TYPE] & 0x1U) ? "HS26 " : "", (e[EXT_CSD_CARD_TYPE] & 0x2U) ? "HS52 " : "",
      (e[EXT_CSD_CARD_TYPE] & 0xCU) ? "DDR " : "", (e[EXT_CSD_CARD_TYPE] & 0x30U) ? "HS200" : "");
  lab_printf("PARTITION_CONFIG 0x%02x", e[EXT_CSD_PARTITION_CONFIG]);
  lab_printf("boot %u x128K, RPMB %u x128K", e[EXT_CSD_BOOT_SIZE_MULT], e[EXT_CSD_RPMB_SIZE_MULT]);
  lab_printf("PWR_CL 52: 0x%02x 26: 0x%02x now 0x%02x", e[EXT_CSD_PWR_CL_52_195],
             e[EXT_CSD_PWR_CL_26_195], e[EXT_CSD_POWER_CLASS]);
  lab_printf("WR_REL_PARAM 0x%02x SET 0x%02x", e[EXT_CSD_WR_REL_PARAM], e[EXT_CSD_WR_REL_SET]);
  lab_printf("REL_WR_SEC_C %u, CMD6 %lu ms", e[EXT_CSD_REL_WR_SEC_C], (unsigned long)s.cmd6_ms);
  lab_printf("HS_TIMING %u BUS_WIDTH %u", e[EXT_CSD_HS_TIMING], e[EXT_CSD_BUS_WIDTH]);

  return s.sec_count >= LAB_GUI_END ? 0 : -EINVAL;
}

static int prv_bus_width(void) {
  int ret = prv_switch(EXT_CSD_BUS_WIDTH, 1U);

  if (ret == 0) {
    pbl_modifyl(SDMMC_CLKCR_WIDBUS_MASK, PBL_FIELD_PREP(SDMMC_CLKCR_WIDBUS_MASK, 1U),
                prv_sd()->base + STM32_SDMMC_CLKCR_OFFSET);
  }

  return ret;
}

// EXT_CSD read on the bus as it runs now, its size the same
static int prv_verify(void) {
  int ret = prv_data(8U, 0U, s.buf, 1U, true);

  if (ret == 0 && prv_sec_count(s.buf) != s.sec_count) {
    lab_printf("EXT_CSD differs");
    ret = -EIO;
  }
  lab_printf("bus %lu Hz: %s", (unsigned long)prv_bus_hz(), ret == 0 ? "good" : "BAD");

  return ret;
}

static int prv_default_speed(void) {
  prv_set_clock(DEFAULT_HZ, SDMMC_CLKCR_PWRSAV);

  return prv_verify();
}

static int prv_high_speed(void) {
  int ret;

  if (!s.hs_card) {
    lab_printf("no HS52 on the card");
    return LAB_SKIP;
  }
  ret = prv_switch(EXT_CSD_HS_TIMING, 1U);
  if (ret != 0) {
    return ret;
  }
  prv_set_clock(HS_HZ, SDMMC_CLKCR_BUSSPEED | SDMMC_CLKCR_PWRSAV);
  ret = prv_verify();
  if (ret != 0) {
    // What the driver falls back to
    lab_printf("HS fails: back to %u Hz", DEFAULT_HZ);
    prv_set_clock(DEFAULT_HZ, SDMMC_CLKCR_PWRSAV);
    ret = prv_verify();
    if (ret == 0) {
      lab_printf("default speed good after HS");
    }
  }

  return ret;
}

static void prv_describe(const uint8_t *b, size_t len) {
  bool ff = true;
  bool zero = true;

  for (size_t i = 0U; i < len; i++) {
    ff &= (b[i] == 0xFFU);
    zero &= (b[i] == 0x00U);
  }
  lab_hex("", b, 16U);
  lab_printf("crc 0x%08" PRIx32 "%s", pbl_crc32(0U, b, len),
             ff ? ", all FF" : (zero ? ", all 00" : ""));
}

static int prv_read_one(void) {
  const uint32_t start = pbl_cycle_get_32();
  int ret = prv_data(17U, LAB_GUI_FIRST, s.buf, 1U, true);

  if (ret == 0) {
    lab_printf("block %u in %lu us", LAB_GUI_FIRST, (unsigned long)prv_us(start));
    prv_describe(s.buf, 512U);
  }

  return ret;
}

static int prv_read_multi(void) {
  uint32_t start;
  uint32_t us;
  int ret;

  ret = prv_cmd(23U, MULTI_BLOCKS, SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  if (ret != 0) {
    return ret;
  }
  start = pbl_cycle_get_32();
  ret = prv_data(18U, LAB_GUI_FIRST, s.buf, MULTI_BLOCKS, true);
  us = prv_us(start);
  if (ret == 0) {
    lab_printf("%u blocks in %lu us", MULTI_BLOCKS, (unsigned long)us);
    lab_printf("= %lu KiB/s",
               (unsigned long)((MULTI_BLOCKS * 512ULL * 1000000ULL / 1024ULL) / PBL_MAX(us, 1U)));
    prv_describe(s.buf, MULTI_BLOCKS * 512U);
  }

  return ret;
}

// One reliable write of a block, as the driver writes: CMD23 with its flag, CMD25, then busy
static int prv_write_block(uint32_t lba, const uint8_t *data, uint32_t *us) {
  const uint32_t start = pbl_cycle_get_32();
  uint32_t polls;
  int ret;

  if (!lab_gui_writable(lba, 1U)) {
    return -EACCES;
  }
  ret = prv_cmd(23U, 1U | PBL_BIT(31), SDMMC_CMD_WAITRESP_SHORT, 0U, nullptr);
  if (ret == 0) {
    ret = prv_data(25U, lba, (void *)data, 1U, false);
  }
  if (ret == 0) {
    ret = prv_wait_ready(BUSY_MS, &polls);
  }
  *us = prv_us(start);

  return ret;
}

static int prv_write_test(void) {
  uint8_t *orig = s.buf;
  uint8_t *pattern = &s.buf[512];
  uint8_t *back = &s.buf[1024];
  const uint32_t lba = LAB_SCRATCH_LBA;
  uint32_t us;
  int ret;

  ret = prv_data(17U, lba, orig, 1U, true);
  if (ret != 0) {
    return ret;
  }
  for (size_t i = 0U; i < 512U; i++) {
    pattern[i] = (uint8_t)(i * 7U + 0x5AU);
  }
  ret = prv_write_block(lba, pattern, &us);
  lab_printf("write %lu: %d, %lu us", (unsigned long)lba, ret, (unsigned long)us);
  if (ret != 0) {
    return ret;
  }
  ret = prv_data(17U, lba, back, 1U, true);
  if (ret == 0) {
    lab_printf("read back: %s", memcmp(pattern, back, 512U) == 0 ? "same" : "DIFFERENT");
  }
  ret = prv_write_block(lba, orig, &us);
  if (ret == 0) {
    ret = prv_data(17U, lba, back, 1U, true);
  }
  if (ret == 0) {
    lab_printf("restored: %s", memcmp(orig, back, 512U) == 0 ? "same" : "DIFFERENT");
  }

  return ret;
}

static int prv_power_off(void) {
  prv_off();
  lab_printf("EMMC_EN off");

  return 0;
}

static void prv_cleanup(void) {
  prv_off();
}

static const struct lab_step s_steps[] = {
  {"kernel clock", prv_clock},
  {"supply on (EMMC_EN PD10)", prv_supply},
  {"host on, 400 kHz", prv_host},
  {"CMD0 go idle", prv_cmd0},
  {"CMD1 operating conditions", prv_cmd1},
  {"CMD2 CID", prv_cmd2},
  {"CMD3 address", prv_cmd3},
  {"CMD9 CSD", prv_cmd9},
  {"CMD7 select", prv_cmd7},
  {"CMD8 EXT_CSD", prv_ext_csd},
  {"bus width 4", prv_bus_width},
  {"default speed", prv_default_speed},
  {"High Speed", prv_high_speed},
  {"read block 409600", prv_read_one},
  {"read 64 blocks", prv_read_multi},
  {"write/read block 600000", prv_write_test},
  {"power off", prv_power_off},
};

const struct lab_experiment lab_exp_emmc_raw = {
  .title = "eMMC step by step",
  .subtitle = "Each command, raw",
  .steps = s_steps,
  .num_steps = PBL_ARRAY_SIZE(s_steps),
  .cleanup = prv_cleanup,
};
