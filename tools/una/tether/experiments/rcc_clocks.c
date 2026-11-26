/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/* Reads the clock tree from RCC, PWR and FLASH and prints it decoded. Read only. */

#include "tether_blob.h"

#define RCC       0x46020c00U
#define PWR       0x46020800U
#define FLASH_ACR 0x40022000U

static const uint32_t s_msi_hz[16] = {48000000, 24000000, 16000000, 12000000, 4000000, 2000000,
                                      1330000,  1000000,  3072000,  1536000,  1024000, 768000,
                                      400000,   200000,   133000,   100000};

static void prv_mhz(const char *name, uint32_t hz) {
  tprintf("  %-10s %4lu.%03lu MHz\n", name, (unsigned long)(hz / 1000000U),
          (unsigned long)((hz / 1000U) % 1000U));
}

static uint32_t prv_ahb_div(uint32_t v) {
  static const uint16_t d[8] = {2, 4, 8, 16, 64, 128, 256, 512};
  return (v & 8U) ? d[v & 7U] : 1U;
}

static uint32_t prv_apb_div(uint32_t v) {
  return (v & 4U) ? (2U << (v & 3U)) : 1U;
}

uint32_t blob_main(const uint32_t *args, uint32_t nargs) {
  const uint32_t cr = REG32(RCC + 0x00U);
  const uint32_t icscr1 = REG32(RCC + 0x08U);
  const uint32_t cfgr1 = REG32(RCC + 0x1cU);
  const uint32_t cfgr2 = REG32(RCC + 0x20U);
  const uint32_t cfgr3 = REG32(RCC + 0x24U);
  const uint32_t pllcfgr = REG32(RCC + 0x28U);
  const uint32_t plldivr = REG32(RCC + 0x34U);
  const uint32_t ccipr1 = REG32(RCC + 0xe0U);
  const uint32_t ccipr2 = REG32(RCC + 0xe4U);
  const uint32_t ccipr3 = REG32(RCC + 0xe8U);
  const uint32_t bdcr = REG32(RCC + 0xf0U);
  const uint32_t csr = REG32(RCC + 0xf4U);

  tprintf("RCC  CR %08lx ICSCR1 %08lx CFGR1 %08lx CFGR2 %08lx CFGR3 %08lx\n", (unsigned long)cr,
          (unsigned long)icscr1, (unsigned long)cfgr1, (unsigned long)cfgr2, (unsigned long)cfgr3);
  tprintf("     PLL1CFGR %08lx PLL1DIVR %08lx CCIPR1 %08lx CCIPR2 %08lx CCIPR3 %08lx\n",
          (unsigned long)pllcfgr, (unsigned long)plldivr, (unsigned long)ccipr1,
          (unsigned long)ccipr2, (unsigned long)ccipr3);
  tprintf("     BDCR %08lx CSR %08lx AHB1ENR %08lx AHB2ENR1 %08lx APB1ENR1 %08lx APB3ENR %08lx\n",
          (unsigned long)bdcr, (unsigned long)csr, (unsigned long)REG32(RCC + 0x88U),
          (unsigned long)REG32(RCC + 0x8cU), (unsigned long)REG32(RCC + 0x9cU),
          (unsigned long)REG32(RCC + 0xa8U));
  tprintf("PWR  CR3 %08lx VOSR %08lx SVMCR %08lx SVMSR %08lx   FLASH ACR %08lx\n",
          (unsigned long)REG32(PWR + 0x08U), (unsigned long)REG32(PWR + 0x0cU),
          (unsigned long)REG32(PWR + 0x10U), (unsigned long)REG32(PWR + 0x3cU),
          (unsigned long)REG32(FLASH_ACR));

  tprintf("oscillators: MSIS %s MSIK %s HSI16 %s HSI48 %s HSE %s%s PLL1 %s LSE %s%s LSI %s\n",
          (cr & 4U) ? "rdy" : "off", (cr & 0x20U) ? "rdy" : "off", (cr & 0x400U) ? "rdy" : "off",
          (cr & 0x2000U) ? "rdy" : "off", (cr & 0x20000U) ? "rdy" : "off",
          (cr & 0x40000U) ? "(byp)" : "", (cr & 0x2000000U) ? "rdy" : "off",
          (bdcr & 2U) ? "rdy" : "off", (bdcr & 4U) ? "(byp)" : "",
          (bdcr & (1U << 27)) ? "rdy" : "off");

  const uint32_t msis_range = (icscr1 & (1U << 23)) ? (icscr1 >> 28) : ((csr >> 12) & 0xfU);
  const uint32_t msik_range = (icscr1 & (1U << 23)) ? ((icscr1 >> 24) & 0xfU) : ((csr >> 8) & 0xfU);
  const uint32_t msis = s_msi_hz[msis_range];
  const uint32_t msik = s_msi_hz[msik_range];
  uint32_t src_hz = 0U;
  switch (pllcfgr & 3U) {
    case 1:
      src_hz = msis;
      break;
    case 2:
      src_hz = 16000000U;
      break;
    case 3:
      src_hz = 16000000U; /* the HSE crystal */
      break;
    default:
      break;
  }
  const uint32_t m = ((pllcfgr >> 8) & 0xfU) + 1U;
  const uint32_t n = (plldivr & 0x1ffU) + 1U;
  const uint64_t vco = (uint64_t)src_hz / m * n;
  const uint32_t p = ((plldivr >> 9) & 0x7fU) + 1U;
  const uint32_t q = ((plldivr >> 16) & 0x7fU) + 1U;
  const uint32_t r = ((plldivr >> 24) & 0x7fU) + 1U;

  tprintf("PLL1: src %lu /M %lu xN %lu  /P %lu%s /Q %lu%s /R %lu%s  MBOOST %lu\n",
          (unsigned long)(pllcfgr & 3U), (unsigned long)m, (unsigned long)n, (unsigned long)p,
          (pllcfgr & (1U << 16)) ? "" : "(off)", (unsigned long)q,
          (pllcfgr & (1U << 17)) ? "" : "(off)", (unsigned long)r,
          (pllcfgr & (1U << 18)) ? "" : "(off)", (unsigned long)((pllcfgr >> 12) & 0xfU));
  prv_mhz("MSIS", msis);
  prv_mhz("MSIK", msik);
  prv_mhz("PLL1 VCO", (uint32_t)vco);
  prv_mhz("PLL1 P", (uint32_t)(vco / p));
  prv_mhz("PLL1 Q", (uint32_t)(vco / q));
  prv_mhz("PLL1 R", (uint32_t)(vco / r));

  static const char *const s_sw[4] = {"MSIS", "HSI16", "HSE", "PLL1 R"};
  uint32_t sys = 0U;
  switch ((cfgr1 >> 2) & 3U) {
    case 0:
      sys = msis;
      break;
    case 1:
    case 2:
      sys = 16000000U;
      break;
    default:
      sys = (uint32_t)(vco / r);
      break;
  }
  const uint32_t hclk = sys / prv_ahb_div(cfgr2 & 0xfU);
  tprintf("SYSCLK from %s\n", s_sw[(cfgr1 >> 2) & 3U]);
  prv_mhz("SYSCLK", sys);
  prv_mhz("HCLK", hclk);
  prv_mhz("PCLK1", hclk / prv_apb_div((cfgr2 >> 4) & 7U));
  prv_mhz("PCLK2", hclk / prv_apb_div((cfgr2 >> 8) & 7U));
  prv_mhz("PCLK3", hclk / prv_apb_div((cfgr3 >> 4) & 7U));
  tprintf("  stub says  %lu Hz\n", (unsigned long)tether->cpu_hz);

  static const char *const s_i2c[4] = {"PCLK", "SYSCLK", "HSI16", "MSIK"};
  static const char *const s_lptim[4] = {"MSIK", "LSI", "HSI16", "LSE"};
  static const char *const s_iclk[4] = {"HSI48", "PLL2Q", "PLL1Q", "MSIK"};
  static const char *const s_vos[4] = {"4", "3", "2", "1"};
  tprintf("kernel clocks: I2C1 %s I2C2 %s I2C4 %s I2C3 %s LPTIM1 %s LPTIM34 %s\n",
          s_i2c[(ccipr1 >> 10) & 3U], s_i2c[(ccipr1 >> 12) & 3U], s_i2c[(ccipr1 >> 14) & 3U],
          s_i2c[(ccipr3 >> 6) & 3U], s_lptim[(ccipr3 >> 10) & 3U], s_lptim[(ccipr3 >> 8) & 3U]);
  tprintf("               ICLK %s SDMMC %s USBPHY %s\n", s_iclk[(ccipr1 >> 26) & 3U],
          (ccipr2 & (1U << 14)) ? "PLL1P" : "ICLK", ((ccipr2 >> 30) & 3U) == 0U ? "HSE" : "other");
  tprintf("voltage range %s, booster %s, flash latency %lu\n",
          s_vos[(REG32(PWR + 0x0cU) >> 16) & 3U], (REG32(PWR + 0x0cU) & (1U << 14)) ? "rdy" : "off",
          (unsigned long)(REG32(FLASH_ACR) & 0xfU));
  tprintf("reset flags now: %s%s%s%s%s%s%s\n", (csr & (1U << 25)) ? "OBL " : "",
          (csr & (1U << 26)) ? "PIN " : "", (csr & (1U << 27)) ? "BOR " : "",
          (csr & (1U << 28)) ? "SFT " : "", (csr & (1U << 29)) ? "IWDG " : "",
          (csr & (1U << 30)) ? "WWDG " : "", (csr & (1U << 31)) ? "LPWR " : "");
  return hclk;
}
