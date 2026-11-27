/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * WRITES THE eMMC, in the GUI region only (sector 600000 and up). Brings the
 * card up as emmc_cid does, prints its write-related EXT_CSD fields, then
 * tries writes and reads them back: CMD24 (one block), CMD23 + CMD25 (two
 * blocks), and CMD23 with the reliable-write flag + CMD25, with the card
 * status after each. The FIFO is fed by the CPU, no DMA.
 *
 * Argument 0 (optional) picks the bus: bit 0 four data lines, bit 1 20 MHz,
 * bit 2 High Speed timing at 40 MHz. Argument 1 (optional) the sector.
 */

#include "tether_blob.h"

#define RCC     0x46020c00U
#define SDMMC   0x420c8000U
#define POWER   (SDMMC + 0x00U)
#define CLKCR   (SDMMC + 0x04U)
#define ARG     (SDMMC + 0x08U)
#define CMD     (SDMMC + 0x0cU)
#define RESP(n) (SDMMC + 0x14U + 4U * (n))
#define DTIMER  (SDMMC + 0x24U)
#define DLEN    (SDMMC + 0x28U)
#define DCTRL   (SDMMC + 0x2cU)
#define STA     (SDMMC + 0x34U)
#define ICR     (SDMMC + 0x38U)
#define MASK    (SDMMC + 0x3cU)
#define FIFO    (SDMMC + 0x80U)

#define STA_CCRCFAIL (1U << 0)
#define STA_DCRCFAIL (1U << 1)
#define STA_CTIMEOUT (1U << 2)
#define STA_DTIMEOUT (1U << 3)
#define STA_RXOVERR  (1U << 5)
#define STA_CMDREND  (1U << 6)
#define STA_CMDSENT  (1U << 7)
#define STA_DATAEND  (1U << 8)
#define STA_RXFIFOHF (1U << 15)
#define STA_RXFIFOE  (1U << 19)
#define STA_BUSYD0   (1U << 20)

#define RESP_NONE  0U
#define RESP_SHORT 1U
#define RESP_R3    2U /* short, no CRC */
#define RESP_LONG  3U

#define RCA 0x0002U

static void prv_gpio_af12(uint32_t port, uint32_t pin, bool pull_up) {
  const uint32_t base = GPIO_PORT(port);
  REG32(base + 0x20U + (pin / 8U) * 4U) =
      (REG32(base + 0x20U + (pin / 8U) * 4U) & ~(0xfU << ((pin % 8U) * 4U))) |
      (12U << ((pin % 8U) * 4U));
  REG32(base + 0x08U) |= 3U << (pin * 2U);
  REG32(base + 0x0cU) =
      (REG32(base + 0x0cU) & ~(3U << (pin * 2U))) | ((pull_up ? 1U : 0U) << (pin * 2U));
  REG32(base + 0x04U) &= ~(1U << pin);
  REG32(base + 0x00U) = (REG32(base + 0x00U) & ~(3U << (pin * 2U))) | (2U << (pin * 2U));
}

static void prv_emmc_power(bool on) {
  const uint32_t d = GPIO_PORT(3);
  REG32(d + 0x18U) = on ? (1U << 10) : (1U << 26);
  REG32(d + 0x00U) = (REG32(d + 0x00U) & ~(3U << 20)) | (1U << 20);
}

/* Returns the status flags of the command, 0 for success */
static uint32_t prv_cmd(uint32_t index, uint32_t arg, uint32_t resp, bool data) {
  const uint32_t start = tether->millis();
  const uint32_t waitresp = resp == RESP_NONE ? 0U : resp == RESP_LONG ? 3U : 1U;

  REG32(ICR) = 0x1fe00fffU;
  REG32(ARG) = arg;
  REG32(CMD) = index | (data ? (1U << 6) : 0U) | (waitresp << 8) | (1U << 12);
  for (;;) {
    const uint32_t sta = REG32(STA);
    if (resp == RESP_NONE && (sta & STA_CMDSENT)) {
      return 0U;
    }
    if (sta & STA_CMDREND) {
      return 0U;
    }
    if (sta & STA_CCRCFAIL) {
      return resp == RESP_R3 ? 0U : STA_CCRCFAIL;
    }
    if (sta & STA_CTIMEOUT) {
      return STA_CTIMEOUT;
    }
    if (tether->millis() - start > 100U) {
      return 0x80000000U;
    }
  }
}

static bool prv_wait_busy(void) {
  const uint32_t start = tether->millis();
  while (REG32(STA) & STA_BUSYD0) {
    if (tether->millis() - start > 500U) {
      return false;
    }
  }
  return true;
}

/* A block of 512 bytes from an adtc command (CMD8, CMD17) */
static uint32_t prv_read_block(uint32_t index, uint32_t arg, uint8_t *buf) {
  uint32_t n = 0U;
  const uint32_t start = tether->millis();

  REG32(DTIMER) = 400000U; /* 1 s at 400 kHz */
  REG32(DLEN) = 512U;
  REG32(DCTRL) = (1U << 1) | (9U << 4); /* card to host, 512-byte blocks */
  uint32_t err = prv_cmd(index, arg, RESP_SHORT, true);
  if (err != 0U) {
    return err;
  }
  for (;;) {
    const uint32_t sta = REG32(STA);
    if (sta & (STA_DCRCFAIL | STA_DTIMEOUT | STA_RXOVERR)) {
      return sta;
    }
    if ((sta & STA_RXFIFOHF) && n + 32U <= 512U) {
      for (unsigned i = 0U; i < 8U; i++, n += 4U) {
        uint32_t w = REG32(FIFO);
        buf[n] = (uint8_t)w;
        buf[n + 1U] = (uint8_t)(w >> 8);
        buf[n + 2U] = (uint8_t)(w >> 16);
        buf[n + 3U] = (uint8_t)(w >> 24);
      }
    } else if (sta & STA_DATAEND) {
      while (!(REG32(STA) & STA_RXFIFOE) && n < 512U) {
        uint32_t w = REG32(FIFO);
        for (unsigned b = 0U; b < 4U; b++) {
          buf[n++] = (uint8_t)(w >> (8U * b));
        }
      }
      break;
    }
    if (tether->millis() - start > 1500U) {
      return 0x80000000U;
    }
  }
  REG32(ICR) = 0x1fe00fffU;
  return n == 512U ? 0U : 0x40000000U;
}

static void prv_hexdump(const uint8_t *buf, uint32_t n) {
  for (uint32_t i = 0U; i < n; i += 16U) {
    tprintf("  %03lx:", (unsigned long)i);
    for (uint32_t j = 0U; j < 16U && i + j < n; j++) {
      tprintf(" %02x", buf[i + j]);
    }
    tprintf("\n");
  }
}

static uint8_t s_block[512];


#define STA_TXUNDERR (1U << 4)

static uint32_t prv_us(void) {
  return tether->cycles() / (tether->cpu_hz / 1000000U);
}
#define STA_TXFIFOHE (1U << 14)
#define TEST_SECTOR  600000U

static uint8_t s_out[1024];

static uint32_t prv_status(void) {
  if (prv_cmd(13U, (uint32_t)RCA << 16, RESP_SHORT, false) != 0U) {
    return 0xffffffffU;
  }
  return REG32(RESP(0));
}

static void prv_print_status(const char *what, uint32_t st) {
  tprintf("  %s status %08lx: state %lu%s%s%s%s%s%s\n", what, (unsigned long)st,
          (unsigned long)((st >> 9) & 0xfU), (st & (1U << 8)) ? " ready" : "",
          (st & (1U << 26)) ? " WP_VIOLATION" : "", (st & (1U << 31)) ? " OUT_OF_RANGE" : "",
          (st & (1U << 22)) ? " ILLEGAL_CMD" : "", (st & (1U << 20)) ? " CC_ERROR" : "",
          (st & (1U << 19)) ? " ERROR" : "");
}

/* Until the card is back in tran with READY_FOR_DATA, or 2 s */
static uint32_t prv_wait_tran(void) {
  const uint32_t start = tether->millis();
  uint32_t st;
  do {
    st = prv_status();
    if (st != 0xffffffffU && ((st >> 9) & 0xfU) == 4U && (st & (1U << 8))) {
      return st;
    }
  } while (tether->millis() - start < 2000U);
  return st;
}

static uint32_t prv_write(uint32_t index, uint32_t arg, const uint8_t *buf, uint32_t blocks,
                          uint32_t *r1, uint32_t *us) {
  const uint32_t total = blocks * 512U;
  uint32_t n = 0U;
  const uint32_t t0 = prv_us();

  REG32(DTIMER) = 0xffffffffU;
  REG32(DLEN) = total;
  REG32(DCTRL) = 9U << 4; /* host to card, 512-byte blocks */
  uint32_t err = prv_cmd(index, arg, RESP_SHORT, true);
  *r1 = REG32(RESP(0));
  if (err != 0U) {
    return err | 0x10000000U;
  }
  const uint32_t start = tether->millis();
  for (;;) {
    const uint32_t sta = REG32(STA);
    if (sta & (STA_DCRCFAIL | STA_DTIMEOUT | STA_TXUNDERR)) {
      *us = prv_us() - t0;
      REG32(ICR) = 0x1fe00fffU;
      return sta;
    }
    if ((sta & STA_TXFIFOHE) && n < total) {
      for (unsigned i = 0U; i < 8U && n < total; i++, n += 4U) {
        REG32(FIFO) = (uint32_t)buf[n] | ((uint32_t)buf[n + 1U] << 8) |
                      ((uint32_t)buf[n + 2U] << 16) | ((uint32_t)buf[n + 3U] << 24);
      }
    } else if (sta & STA_DATAEND) {
      break;
    }
    if (tether->millis() - start > 3000U) {
      *us = prv_us() - t0;
      return 0x80000000U | sta;
    }
  }
  *us = prv_us() - t0;
  REG32(ICR) = 0x1fe00fffU;
  return 0U;
}

static void prv_test(const char *name, uint32_t sector, uint32_t blocks, bool use23,
                     bool reliable, uint8_t seed) {
  uint32_t r1 = 0U, us = 0U, err;

  if (!tether_emmc_write_allowed(sector, blocks)) {
    tprintf("%s: sector %lu outside the GUI region, skipped\n", name, (unsigned long)sector);
    return;
  }
  for (uint32_t i = 0U; i < blocks * 512U; i++) {
    s_out[i] = (uint8_t)(seed + i * 7U);
  }
  tprintf("%s: sector %lu, %lu block(s)\n", name, (unsigned long)sector, (unsigned long)blocks);
  if (use23) {
    err = prv_cmd(23U, blocks | (reliable ? (1U << 31) : 0U), RESP_SHORT, false);
    if (err != 0U) {
      tprintf("  CMD23 failed: %08lx, R1 %08lx\n", (unsigned long)err,
              (unsigned long)REG32(RESP(0)));
      prv_print_status("after CMD23", prv_status());
      return;
    }
  }
  err = prv_write(blocks > 1U || use23 ? 25U : 24U, sector, s_out, blocks, &r1, &us);
  tprintf("  write: STA/err %08lx, R1 %08lx, %lu us\n", (unsigned long)err, (unsigned long)r1,
          (unsigned long)us);
  if (err != 0U) {
    (void)prv_cmd(12U, 0U, RESP_SHORT, false);
    REG32(DCTRL) = 1U << 13; /* FIFORST */
    REG32(ICR) = 0x1fe00fffU;
  }
  const uint32_t t0 = prv_us();
  const uint32_t st = prv_wait_tran();
  tprintf("  busy for %lu us\n", (unsigned long)(prv_us() - t0));
  prv_print_status("after", st);

  uint32_t bad = 0U;
  for (uint32_t b = 0U; b < blocks; b++) {
    if ((err = prv_read_block(17U, sector + b, s_block)) != 0U) {
      tprintf("  read back block %lu failed: %08lx\n", (unsigned long)b, (unsigned long)err);
      return;
    }
    for (uint32_t i = 0U; i < 512U; i++) {
      bad += s_block[i] != s_out[b * 512U + i];
    }
  }
  tprintf("  read back: %s (%lu bytes differ)\n", bad ? "MISMATCH" : "ok", (unsigned long)bad);
}

static uint32_t prv_switch(uint32_t index, uint32_t value) {
  uint32_t err = prv_cmd(6U, (3U << 24) | (index << 16) | (value << 8), RESP_SHORT, false);
  if (err == 0U && !prv_wait_busy()) {
    err = 0x20000000U;
  }
  const uint32_t st = prv_status();
  tprintf("CMD6 EXT_CSD[%lu] = %lu: err %08lx\n", (unsigned long)index, (unsigned long)value,
          (unsigned long)err);
  prv_print_status("after CMD6", st);
  return err;
}

static uint32_t prv_run(const uint32_t *args, uint32_t nargs) {
  uint32_t err;
  const uint32_t mode = nargs > 0U ? args[0] : 0U;
  const uint32_t sector = nargs > 1U ? args[1] : TEST_SECTOR;

  const bool pll_q = (REG32(RCC + 0x00U) & (1U << 25)) && (REG32(RCC + 0x28U) & (1U << 17));
  uint32_t kernel_hz = 80000000U;
  if (!pll_q) {
    REG32(RCC + 0x00U) |= 1U << 12;
    while (!(REG32(RCC + 0x00U) & (1U << 13))) {
    }
    kernel_hz = 48000000U;
  }
  REG32(RCC + 0xe0U) = (REG32(RCC + 0xe0U) & ~(3U << 26)) | ((pll_q ? 2U : 0U) << 26);
  REG32(RCC + 0xe4U) &= ~(1U << 14);
  REG32(RCC + 0x8cU) |= (1U << 27) | (1U << 2) | (1U << 3);
  (void)REG32(RCC + 0x8cU);
  REG32(RCC + 0x64U) |= 1U << 27;
  (void)REG32(RCC + 0x64U);
  REG32(RCC + 0x64U) &= ~(1U << 27);

  for (uint32_t pin = 8U; pin <= 11U; pin++) {
    prv_gpio_af12(2U, pin, true);
  }
  prv_gpio_af12(2U, 12U, false);
  prv_gpio_af12(3U, 2U, true);

  tprintf("emmc_write_test: WRITES the eMMC GUI region, mode %lu, kernel %lu MHz\n",
          (unsigned long)mode, (unsigned long)(kernel_hz / 1000000U));
  prv_emmc_power(true);
  tether_delay_ms(50U);

  REG32(CLKCR) = (kernel_hz / (2U * 400000U)) | (1U << 17);
  REG32(POWER) = 3U;
  tether_delay_ms(2U);
  REG32(MASK) = 0U;
  (void)prv_cmd(0U, 0U, RESP_NONE, false);
  tether_delay_ms(2U);

  uint32_t ocr = 0U;
  const uint32_t start = tether->millis();
  do {
    if ((err = prv_cmd(1U, 0x40ff8080U, RESP_R3, false)) != 0U) {
      tprintf("CMD1 failed: %08lx\n", (unsigned long)err);
      return 1U;
    }
    ocr = REG32(RESP(0));
    if (tether->millis() - start > 1500U) {
      tprintf("CMD1 busy\n");
      return 2U;
    }
    if ((ocr & 0x80000000U) == 0U) {
      tether_delay_ms(2U);
    }
  } while ((ocr & 0x80000000U) == 0U);
  if (prv_cmd(2U, 0U, RESP_LONG, false) || prv_cmd(3U, (uint32_t)RCA << 16, RESP_SHORT, false) ||
      prv_cmd(7U, (uint32_t)RCA << 16, RESP_SHORT, false) || !prv_wait_busy()) {
    tprintf("identification failed\n");
    return 3U;
  }
  if ((err = prv_read_block(8U, 0U, s_block)) != 0U) {
    tprintf("EXT_CSD read failed: %08lx\n", (unsigned long)err);
    return 4U;
  }
  tprintf("EXT_CSD: WR_REL_PARAM[166] %02x WR_REL_SET[167] %02x REL_WR_SEC_C[222] %02x\n",
          s_block[166], s_block[167], s_block[222]);
  tprintf("  USER_WP[171] %02x BOOT_WP[173] %02x ERASE_GRP_DEF[175] %02x PART_CFG[179] %02x\n",
          s_block[171], s_block[172 + 1], s_block[175], s_block[179]);
  tprintf("  PARTITION_SETTING_COMPLETED[155] %02x CACHE_CTRL[33] %02x CACHE_SIZE %02x%02x%02x%02x\n",
          s_block[155], s_block[33], s_block[252], s_block[251], s_block[250], s_block[249]);
  tprintf("  HS_TIMING[185] %02x BUS_WIDTH[183] %02x DEVICE_TYPE[196] %02x DRIVER_STRENGTH[197] %02x\n",
          s_block[185], s_block[183], s_block[196], s_block[197]);
  prv_print_status("selected", prv_status());

  if (mode & 1U) {
    if (prv_switch(183U, 1U) == 0U) {
      REG32(CLKCR) = (REG32(CLKCR) & ~(3U << 14)) | (1U << 14);
    }
  }
  if (mode & 4U) {
    if (prv_switch(185U, 1U) == 0U) {
      REG32(CLKCR) = (REG32(CLKCR) & ~0x3ffU) | (kernel_hz / (2U * 40000000U));
    }
  } else if (mode & 2U) {
    REG32(CLKCR) = (REG32(CLKCR) & ~0x3ffU) | (kernel_hz / (2U * 20000000U));
  }
  tprintf("CLKCR %08lx\n", (unsigned long)REG32(CLKCR));

  prv_test("CMD24", sector, 1U, false, false, 0x11U);
  prv_test("CMD23+CMD25", sector + 2U, 2U, true, false, 0x22U);
  prv_test("CMD23 reliable+CMD25", sector + 4U, 2U, true, true, 0x33U);
  prv_test("CMD23 reliable+CMD25, 1 block", sector + 6U, 1U, true, true, 0x44U);

  (void)prv_cmd(7U, 0U, RESP_NONE, false);
  return 0U;
}

uint32_t blob_main(const uint32_t *args, uint32_t nargs) {
  const uint32_t ret = prv_run(args, nargs);

  REG32(POWER) = 0U;
  REG32(RCC + 0x8cU) &= ~(1U << 27);
  prv_emmc_power(false);
  tprintf("eMMC: power off\n");
  return ret;
}
