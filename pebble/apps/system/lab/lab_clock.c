/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The clock tree: what it is, B's PLL1 Q at 80 MHz applied at runtime, and the eMMC's kernel
// clock.

#include "lab.h"

#include <inttypes.h>

#include <pbl/device.h>
#include <pbl/drivers/clk.h>
#include <pbl/drivers/mmc.h>
#include <pbl/drivers/mmc/stm32_sdmmc.h>
#include <pbl/kernel/mmio.h>
#include <pbl/util/bits.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#include <devicetree/devices.h>
#include <devicetree/regs.h>
#include <dt-bindings/clock/st,stm32u5-rcc.h>
#include <hardware/stm32u5_flash.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>

#define RCC(reg) (PBL_DT_REG_RCC + STM32U5_RCC_##reg##_OFFSET)
#define CLK(id)  ((struct pbl_clk)PBL_CLK(&pbl_clk_stm32u5, STM32U5_CLK_##id))

#define MHZ_MILLI(mhz) ((uint64_t)(mhz) * 1000000ULL * 1000ULL)

static bool s_pll1_q;

static void prv_rate(const char *name, struct pbl_clk clk) {
  uint64_t rate;

  if (pbl_clk_get_rate(&clk, &rate) == 0) {
    lab_printf("%s %lu.%03lu MHz", name, (unsigned long)(rate / 1000000000ULL),
               (unsigned long)((rate / 1000000ULL) % 1000ULL));
  } else {
    lab_printf("%s ?", name);
  }
}

static int prv_show(void) {
  static const char *const s_sws[] = {"MSIS", "HSI16", "HSE", "PLL1"};
  static const char *const s_src[] = {"none", "MSIS", "HSI16", "HSE"};
  static const char *const s_iclk[] = {"HSI48", "PLL2 Q", "PLL1 Q", "MSIK"};
  const uint32_t cr = pbl_readl(RCC(CR));
  const uint32_t cfgr1 = pbl_readl(RCC(CFGR1));
  const uint32_t pllcfgr = pbl_readl(RCC(PLL1CFGR));
  const uint32_t plldivr = pbl_readl(RCC(PLL1DIVR));
  const uint32_t ccipr1 = pbl_readl(RCC(CCIPR1));
  const uint32_t ccipr2 = pbl_readl(RCC(CCIPR2));
  const uint32_t vosr = pbl_readl(PBL_DT_REG_PWR + STM32U5_PWR_VOSR_OFFSET);
  const uint32_t acr = pbl_readl(PBL_DT_REG_FLASH + STM32U5_FLASH_ACR_OFFSET);

  lab_printf("SYSCLK on %s", s_sws[PBL_FIELD_GET(RCC_CFGR1_SWS_MASK, cfgr1)]);
  lab_printf("PLL1 %s%s, from %s", (cr & RCC_CR_PLL1ON) ? "on" : "off",
             (cr & RCC_CR_PLL1RDY) ? " ready" : "",
             s_src[PBL_FIELD_GET(RCC_PLL1CFGR_SRC_MASK, pllcfgr)]);
  lab_printf("M %lu N %lu", (unsigned long)PBL_FIELD_GET(RCC_PLL1CFGR_M_MASK, pllcfgr) + 1UL,
             (unsigned long)PBL_FIELD_GET(RCC_PLL1DIVR_N_MASK, plldivr) + 1UL);
  lab_printf("P/%lu%s Q/%lu%s R/%lu%s",
             (unsigned long)PBL_FIELD_GET(RCC_PLL1DIVR_P_MASK, plldivr) + 1UL,
             (pllcfgr & RCC_PLL1CFGR_PEN) ? "+" : "-",
             (unsigned long)PBL_FIELD_GET(RCC_PLL1DIVR_Q_MASK, plldivr) + 1UL,
             (pllcfgr & RCC_PLL1CFGR_QEN) ? "+" : "-",
             (unsigned long)PBL_FIELD_GET(RCC_PLL1DIVR_R_MASK, plldivr) + 1UL,
             (pllcfgr & RCC_PLL1CFGR_REN) ? "+" : "-");
  prv_rate("SYSCLK", CLK(SYSCLK));
  prv_rate("PLL1 Q", CLK(PLL1_Q));
  lab_printf("ICLK %s, SDMMC %s", s_iclk[PBL_FIELD_GET(RCC_CCIPR1_ICLKSEL_MASK, ccipr1)],
             (ccipr2 & RCC_CCIPR2_SDMMCSEL) ? "PLL1 P" : "ICLK");
  prv_rate("SDMMC ker", CLK(SDMMC1));
  lab_printf("HSI48 %s, HSE %s", (cr & RCC_CR_HSI48RDY) ? "on" : "off",
             (cr & RCC_CR_HSERDY) ? "on" : "off");
  lab_printf("VOS%lu boost %s%s", 4UL - (unsigned long)PBL_FIELD_GET(PWR_VOSR_VOS_MASK, vosr),
             (vosr & PWR_VOSR_BOOSTEN) ? "on" : "off", (vosr & PWR_VOSR_BOOSTRDY) ? " rdy" : "");
  lab_printf("Flash latency %lu", (unsigned long)PBL_FIELD_GET(FLASH_ACR_LATENCY_MASK, acr));
  lab_printf("CR 0x%08" PRIx32, cr);
  lab_printf("PLL1CFGR 0x%08" PRIx32, pllcfgr);
  lab_printf("PLL1DIVR 0x%08" PRIx32, plldivr);
#ifdef CONFIG_SOC_STM32U5_EARLY_PLL1_OFF
  lab_printf("Early PLL1 off: on");
#else
  lab_printf("Early PLL1 off: off (as A)");
#endif
  lab_printf("eMMC clock: %s", s_pll1_q ? "PLL1 Q (B)" : "HSI48 (Lab)");

  return 0;
}

bool lab_emmc_clock_is_pll1_q(void) {
  return s_pll1_q;
}

// The driver takes its kernel clock's rate at its init: done again, the card off
static int prv_driver_reinit(void) {
  int ret;

  // Its device fails at boot only if the clock tree or a pin does
  lab_printf("MMC device %s", pbl_device_is_ready(&MMC->dev) ? "ready" : "NOT READY");

  pbl_mmc_power_off(MMC);
  ret = MMC->ops->init(MMC);
  if (ret == 0) {
    const struct pbl_mmc_stm32_sdmmc *sd =
        PBL_CONTAINER_OF(MMC, const struct pbl_mmc_stm32_sdmmc, mmc);

    lab_printf("driver: kernel clock %lu Hz", (unsigned long)sd->state->ker_hz);
  }

  return ret;
}

int lab_emmc_clock_select(bool pll1_q) {
  struct pbl_clk iclk = CLK(ICLK);
  struct pbl_clk sdmmc = CLK(SDMMC1);
  struct pbl_clk parent = pll1_q ? CLK(PLL1_Q) : CLK(HSI48);
  int ret;

  ret = pbl_clk_set_parent(&iclk, &parent);
  if (ret == 0) {
    ret = pbl_clk_set_parent(&sdmmc, &iclk);
  }
  if (ret != 0) {
    lab_printf("clock select: %d", ret);
    return ret;
  }
  s_pll1_q = pll1_q;

  return prv_driver_reinit();
}

static int prv_sys_msis(void) {
  struct pbl_clk sys = CLK(SYSCLK);
  struct pbl_clk msis = CLK(MSIS);

  return pbl_clk_set_parent(&sys, &msis);
}

static int prv_pll1_q(void) {
  struct pbl_clk q = CLK(PLL1_Q);
  int ret;

  ret = pbl_clk_set_rate(&q, MHZ_MILLI(80));
  prv_rate("PLL1 Q", q);

  return ret;
}

static int prv_sys_pll1(void) {
  struct pbl_clk sys = CLK(SYSCLK);
  struct pbl_clk r = CLK(PLL1_R);
  int ret;

  ret = pbl_clk_set_parent(&sys, &r);
  prv_rate("SYSCLK", sys);

  return ret;
}

static int prv_select_pll1_q(void) {
  return lab_emmc_clock_select(true);
}

static int prv_select_hsi48(void) {
  return lab_emmc_clock_select(false);
}

static const struct lab_step s_show_steps[] = {
  {"clock tree", prv_show},
};

const struct lab_experiment lab_exp_clock_show = {
  .title = "Clocks",
  .subtitle = "PLL1, eMMC clock, VOS",
  .steps = s_show_steps,
  .num_steps = PBL_ARRAY_SIZE(s_show_steps),
};

static const struct lab_step s_b_steps[] = {
  {"before", prv_show},
  {"SYSCLK to MSIS", prv_sys_msis},
  {"PLL1 off, Q 80 MHz, on", prv_pll1_q},
  {"SYSCLK back to PLL1 R", prv_sys_pll1},
  {"eMMC on ICLK = PLL1 Q", prv_select_pll1_q},
  {"after", prv_show},
};

const struct lab_experiment lab_exp_clock_b = {
  .title = "B's clocks",
  .subtitle = "PLL1 Q 80 MHz, at runtime",
  .steps = s_b_steps,
  .num_steps = PBL_ARRAY_SIZE(s_b_steps),
  .confirm =
      "Runs the CPU on MSIS, sets PLL1 up again with Q at 80 MHz, as B does at boot, and "
      "the eMMC on it until the next boot.",
};

static const struct lab_step s_hsi48_steps[] = {
  {"eMMC on ICLK = HSI48", prv_select_hsi48},
  {"after", prv_show},
};

const struct lab_experiment lab_exp_clock_hsi48 = {
  .title = "eMMC on HSI48",
  .subtitle = "The Lab's eMMC clock",
  .steps = s_hsi48_steps,
  .num_steps = PBL_ARRAY_SIZE(s_hsi48_steps),
};
