/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Clocks. The bootloader jumps here with its clock tree as it used it: the
 * system clock goes to MSIS first, then to PLL1 at 160 MHz as the stock
 * kernel and PebbleOS run it (PLL1 Q at 80 MHz for the eMMC of experiments).
 * If PLL1 does not lock, the stub stays on MSIS at 48 MHz, which still runs
 * the USB.
 */

#include "regs.h"
#include "stub.h"

struct clock_state g_clock;
volatile uint32_t g_ms;

/* Iterations of a ready poll before the tick runs: far longer than any oscillator takes */
#define POLL_LOOPS 2000000U

static bool prv_wait_set(uintptr_t reg, uint32_t mask, uint32_t loops) {
  while ((rd(reg) & mask) == 0U) {
    if (loops-- == 0U) {
      return false;
    }
  }
  return true;
}

static bool prv_wait_clear(uintptr_t reg, uint32_t mask, uint32_t loops) {
  while ((rd(reg) & mask) != 0U) {
    if (loops-- == 0U) {
      return false;
    }
  }
  return true;
}

static void prv_sysclk(uint32_t sw) {
  modb(RCC(STM32U5_RCC_CFGR1_OFFSET), RCC_CFGR1_SW_MASK, sw);
  for (uint32_t n = POLL_LOOPS; n != 0U; n--) {
    if (((rd(RCC(STM32U5_RCC_CFGR1_OFFSET)) & RCC_CFGR1_SWS_MASK) >> RCC_CFGR1_SWS_SHIFT) == sw) {
      return;
    }
  }
}

static bool prv_lse_on(void) {
  uintptr_t bdcr = RCC(STM32U5_RCC_BDCR_OFFSET);

  setb(PWR(STM32U5_PWR_DBPR_OFFSET), PWR_DBPR_DBP);
  if ((rd(bdcr) & RCC_BDCR_LSERDY) == 0U) {
    /* An external 32.768 kHz clock on PC14: bypass */
    clrb(bdcr, RCC_BDCR_LSEON);
    setb(bdcr, RCC_BDCR_LSEBYP);
    setb(bdcr, RCC_BDCR_LSEON);
    if (!prv_wait_set(bdcr, RCC_BDCR_LSERDY, POLL_LOOPS * 4U)) {
      return false;
    }
  }
  setb(bdcr, RCC_BDCR_LSESYSEN);
  return prv_wait_set(bdcr, RCC_BDCR_LSESYSRDY, POLL_LOOPS);
}

void clock_init(void) {
  uintptr_t vosr = PWR(STM32U5_PWR_VOSR_OFFSET);

  /* The cycle counter, for delays */
  setb(SCB_DEMCR, 1U << 24);
  wr(DWT_CYCCNT, 0U);
  setb(DWT_CTRL, 1U);

  setb(RCC(STM32U5_RCC_AHB3ENR_OFFSET), RCC_AHB3ENR_PWREN);
  setb(RCC(STM32U5_RCC_AHB1ENR_OFFSET), RCC_AHB1ENR_FLASHEN);
  (void)rd(RCC(STM32U5_RCC_AHB1ENR_OFFSET));

  /* Enough wait states for any clock, before any change */
  modb(FLASHR(STM32U5_FLASH_ACR_OFFSET), FLASH_ACR_LATENCY_MASK, 4U);
  while ((rd(FLASHR(STM32U5_FLASH_ACR_OFFSET)) & FLASH_ACR_LATENCY_MASK) != 4U) {
  }

  /* MSIS runs everything while PLL1 is set up again, at whatever range it has */
  setb(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_MSISON);
  (void)prv_wait_set(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_MSISRDY, POLL_LOOPS);
  prv_sysclk(RCC_CFGR1_SW_MSIS);
  modb(RCC(STM32U5_RCC_CFGR2_OFFSET),
       RCC_CFGR2_HPRE_MASK | RCC_CFGR2_PPRE1_MASK | RCC_CFGR2_PPRE2_MASK, 0U);
  modb(RCC(STM32U5_RCC_CFGR3_OFFSET), RCC_CFGR3_PPRE3_MASK, 0U);
  clrb(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_PLL1ON);
  (void)prv_wait_clear(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_PLL1RDY, POLL_LOOPS);

  /* Range 1, the booster off while its prescaler changes */
  modb(vosr, PWR_VOSR_VOS_MASK | PWR_VOSR_BOOSTEN,
       PBL_FIELD_PREP(PWR_VOSR_VOS_MASK, PWR_VOSR_VOS_1));
  (void)prv_wait_set(vosr, PWR_VOSR_VOSRDY, POLL_LOOPS);
  (void)prv_wait_set(PWR(STM32U5_PWR_SVMSR_OFFSET), PWR_SVMSR_ACTVOSRDY, POLL_LOOPS);

  /* MSIS at 48 MHz */
  modb(RCC(STM32U5_RCC_ICSCR1_OFFSET), RCC_ICSCR1_MSISRANGE_MASK, RCC_ICSCR1_MSIRGSEL);
  (void)prv_wait_set(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_MSISRDY, POLL_LOOPS);
  g_clock.sysclk_hz = 48000000U;

  /* PLL1 from MSIS / 3 = 16 MHz: x10 = 160 MHz VCO, R / 1, Q / 2, P / 2; booster at 12 MHz */
  wr(RCC(STM32U5_RCC_PLL1CFGR_OFFSET),
     PBL_FIELD_PREP(RCC_PLL1CFGR_SRC_MASK, RCC_PLL1CFGR_SRC_MSIS) |
         PBL_FIELD_PREP(RCC_PLL1CFGR_RGE_MASK, RCC_PLL1CFGR_RGE_8_16MHZ) |
         PBL_FIELD_PREP(RCC_PLL1CFGR_M_MASK, 2U) | PBL_FIELD_PREP(RCC_PLL1CFGR_MBOOST_MASK, 2U) |
         RCC_PLL1CFGR_QEN | RCC_PLL1CFGR_REN);
  wr(RCC(STM32U5_RCC_PLL1DIVR_OFFSET),
     PBL_FIELD_PREP(RCC_PLL1DIVR_N_MASK, 9U) | PBL_FIELD_PREP(RCC_PLL1DIVR_P_MASK, 1U) |
         PBL_FIELD_PREP(RCC_PLL1DIVR_Q_MASK, 1U) | PBL_FIELD_PREP(RCC_PLL1DIVR_R_MASK, 0U));
  wr(RCC(0x38U), 0U); /* PLL1FRACR */

  /* The booster, which range 1 needs above 55 MHz and the USB transceiver supply needs too */
  setb(vosr, PWR_VOSR_BOOSTEN);
  bool boost = prv_wait_set(vosr, PWR_VOSR_BOOSTRDY, POLL_LOOPS);

  setb(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_PLL1ON);
  if (boost && prv_wait_set(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_PLL1RDY, POLL_LOOPS)) {
    prv_sysclk(RCC_CFGR1_SW_PLL1);
    if (((rd(RCC(STM32U5_RCC_CFGR1_OFFSET)) & RCC_CFGR1_SWS_MASK) >> RCC_CFGR1_SWS_SHIFT) ==
        RCC_CFGR1_SW_PLL1) {
      g_clock.pll = true;
      g_clock.sysclk_hz = 160000000U;
    }
  }

  /* Port G is on VDDIO2, isolated until declared valid */
  setb(PWR(STM32U5_PWR_SVMCR_OFFSET), PWR_SVMCR_IO2SV);

  /* HSI16 clocks the I2C */
  setb(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_HSION);
  (void)prv_wait_set(RCC(STM32U5_RCC_CR_OFFSET), RCC_CR_HSIRDY, POLL_LOOPS);

  g_clock.lse = prv_lse_on();
  if (!g_clock.lse) {
    /* LSI for the display's VCOM instead */
    setb(RCC(STM32U5_RCC_BDCR_OFFSET), RCC_BDCR_LSION);
    (void)prv_wait_set(RCC(STM32U5_RCC_BDCR_OFFSET), RCC_BDCR_LSIRDY, POLL_LOOPS);
  }

  /* A 1 kHz tick, the highest priority but the faults */
  wr(SYST_CSR, 0U);
  wr(SYST_RVR, (g_clock.sysclk_hz / 1000U) - 1U);
  wr(SYST_CVR, 0U);
  modb(SCB_SHPR3, 0xffU << 24, 0U);
  wr(SYST_CSR, 7U);
}

/* The HSE crystal clocks the USB PHY only */
bool clock_hse_on(void) {
  uintptr_t cr = RCC(STM32U5_RCC_CR_OFFSET);

  if ((rd(cr) & RCC_CR_HSERDY) != 0U && (rd(cr) & RCC_CR_HSEBYP) == 0U) {
    g_clock.hse = true;
    return true;
  }
  clrb(cr, RCC_CR_HSEON);
  (void)prv_wait_clear(cr, RCC_CR_HSERDY, POLL_LOOPS);
  clrb(cr, RCC_CR_HSEBYP);
  setb(cr, RCC_CR_HSEON);
  uint32_t start = millis();
  while ((rd(cr) & RCC_CR_HSERDY) == 0U) {
    if (millis() - start > 100U) {
      g_clock.hse = false;
      return false;
    }
  }
  g_clock.hse = true;
  return true;
}

uint32_t millis(void) {
  return g_ms;
}

uint32_t cycles(void) {
  return rd(DWT_CYCCNT);
}

void delay_us(uint32_t us) {
  const uint32_t per_us = g_clock.sysclk_hz / 1000000U;

  while (us > 10000U) {
    delay_us(10000U);
    us -= 10000U;
  }
  uint32_t start = cycles();
  while ((cycles() - start) < us * per_us) {
  }
}

void delay_ms(uint32_t ms) {
  while (ms-- != 0U) {
    delay_us(1000U);
    watchdog_feed();
  }
}

/* Harmless when the watchdog does not run, which the bootloader does not seem to start */
void watchdog_feed(void) {
  wr(IWDG_BASE + STM32_IWDG_KR_OFFSET, IWDG_KR_RELOAD);
}
