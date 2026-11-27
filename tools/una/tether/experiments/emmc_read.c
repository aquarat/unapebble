/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Powers the eMMC (EMMC_EN, PD10), brings it up on SDMMC1 at 400 kHz on one
 * data line, and prints its CID, CSD and the main fields of its EXT_CSD.
 * With an argument, also reads that sector (CMD17), or argument 1 sectors from
 * it, and prints them whole. Read only: no command here writes the card. Powers it off after.
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

static uint32_t prv_run(const uint32_t *args, uint32_t nargs) {
  uint32_t err;
  uint32_t cid[4], csd[4];

  /* SDMMC1 on ICLK: PLL1 Q at 80 MHz, as the stub sets it up, HSI48 otherwise */
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
    prv_gpio_af12(2U, pin, true); /* PC8-PC11: D0-D3 */
  }
  prv_gpio_af12(2U, 12U, false); /* PC12: CK */
  prv_gpio_af12(3U, 2U, true);   /* PD2: CMD */

  tprintf("eMMC: power on, SDMMC1 from %s\n", pll_q ? "PLL1 Q" : "HSI48");
  prv_emmc_power(true);
  tether_delay_ms(50U);

  REG32(CLKCR) = (kernel_hz / (2U * 400000U)) | (1U << 17); /* 400 kHz, 1 bit, flow control */
  REG32(POWER) = 3U;
  tether_delay_ms(2U);
  REG32(MASK) = 0U;

  (void)prv_cmd(0U, 0U, RESP_NONE, false);
  tether_delay_ms(2U);

  /* CMD1 until the card is ready: sector addressing, 2.7-3.6 V and 1.7-1.95 V */
  uint32_t ocr = 0U;
  const uint32_t start = tether->millis();
  do {
    err = prv_cmd(1U, 0x40ff8080U, RESP_R3, false);
    if (err != 0U) {
      tprintf("CMD1 failed: %08lx\n", (unsigned long)err);
      return 1U;
    }
    ocr = REG32(RESP(0));
    if (tether->millis() - start > 1500U) {
      tprintf("CMD1: still busy after 1.5 s, OCR %08lx\n", (unsigned long)ocr);
      return 2U;
    }
    if ((ocr & 0x80000000U) == 0U) {
      tether_delay_ms(2U);
    }
  } while ((ocr & 0x80000000U) == 0U);
  tprintf("OCR %08lx (%s addressing)\n", (unsigned long)ocr,
          (ocr & 0x40000000U) ? "sector" : "byte");

  if ((err = prv_cmd(2U, 0U, RESP_LONG, false)) != 0U) {
    tprintf("CMD2 failed: %08lx\n", (unsigned long)err);
    return 3U;
  }
  for (unsigned i = 0U; i < 4U; i++) {
    cid[i] = REG32(RESP(i));
  }
  if ((err = prv_cmd(3U, (uint32_t)RCA << 16, RESP_SHORT, false)) != 0U) {
    tprintf("CMD3 failed: %08lx\n", (unsigned long)err);
    return 4U;
  }
  if ((err = prv_cmd(9U, (uint32_t)RCA << 16, RESP_LONG, false)) != 0U) {
    tprintf("CMD9 failed: %08lx\n", (unsigned long)err);
    return 5U;
  }
  for (unsigned i = 0U; i < 4U; i++) {
    csd[i] = REG32(RESP(i));
  }

  /* CID: MID [127:120], CBX [113:112], OID [111:104], PNM [103:56], PRV [55:48], PSN [47:16],
     MDT [15:8] */
  char pnm[7];
  for (unsigned i = 0U; i < 6U; i++) {
    const unsigned bit = 103U - 8U * i;
    uint8_t c = (uint8_t)(cid[(127U - bit) / 32U] >> (bit % 32U - 7U));
    pnm[i] = (c >= 0x20U && c < 0x7fU) ? (char)c : '?';
  }
  pnm[6] = '\0';
  tprintf("CID %08lx %08lx %08lx %08lx\n", (unsigned long)cid[0], (unsigned long)cid[1],
          (unsigned long)cid[2], (unsigned long)cid[3]);
  tprintf("  manufacturer %02lx, OEM %02lx, product \"%s\", revision %lu.%lu\n",
          (unsigned long)(cid[0] >> 24), (unsigned long)((cid[0] >> 8) & 0xffU), pnm,
          (unsigned long)((cid[2] >> 20) & 0xfU), (unsigned long)((cid[2] >> 16) & 0xfU));
  tprintf("  serial %08lx, made %lu/%lu (MDT %02lx)\n",
          (unsigned long)((cid[2] << 16) | (cid[3] >> 16)), (unsigned long)((cid[3] >> 12) & 0xfU),
          (unsigned long)(((cid[3] >> 8) & 0xfU) + 1997U), (unsigned long)((cid[3] >> 8) & 0xffU));
  tprintf("CSD %08lx %08lx %08lx %08lx\n", (unsigned long)csd[0], (unsigned long)csd[1],
          (unsigned long)csd[2], (unsigned long)csd[3]);

  if ((err = prv_cmd(7U, (uint32_t)RCA << 16, RESP_SHORT, false)) != 0U || !prv_wait_busy()) {
    tprintf("CMD7 failed: %08lx\n", (unsigned long)err);
    return 6U;
  }
  if ((err = prv_read_block(8U, 0U, s_block)) != 0U) {
    tprintf("CMD8 (EXT_CSD) failed: %08lx\n", (unsigned long)err);
    return 7U;
  }
  const uint32_t sectors = (uint32_t)s_block[212] | ((uint32_t)s_block[213] << 8) |
                           ((uint32_t)s_block[214] << 16) | ((uint32_t)s_block[215] << 24);
  tprintf("EXT_CSD rev %u, %lu sectors (%lu MiB), card type %02x\n", s_block[192],
          (unsigned long)sectors, (unsigned long)(sectors / 2048U), s_block[196]);
  tprintf("  life time A %02x B %02x, pre-EOL %02x, boot size %u x 128 KiB, partition cfg %02x\n",
          s_block[268], s_block[269], s_block[267], s_block[226], s_block[179]);
  tprintf("  bus width %u, HS timing %u, firmware %02x%02x%02x%02x%02x%02x%02x%02x\n", s_block[183],
          s_block[185], s_block[261], s_block[260], s_block[259], s_block[258], s_block[257],
          s_block[256], s_block[255], s_block[254]);

  if (nargs > 0U) {
    const uint32_t sector = args[0];
    if ((err = prv_read_block(17U, sector, s_block)) != 0U) {
      tprintf("CMD17 sector %lu failed: %08lx\n", (unsigned long)sector, (unsigned long)err);
      return 8U;
    }
    const uint32_t count = nargs > 1U ? args[1] : 1U;
    for (uint32_t k = 0U; k < count; k++) {
      if (k > 0U && (err = prv_read_block(17U, sector + k, s_block)) != 0U) {
        tprintf("CMD17 sector %lu failed: %08lx\n", (unsigned long)(sector + k), (unsigned long)err);
        return 8U;
      }
      tprintf("sector %lu:\n", (unsigned long)(sector + k));
      prv_hexdump(s_block, 512U);
    }
  }

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
