/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/delay.h>
#include <pbl/kernel/idle.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/time.h>
#include <pbl/soc/st/idle.h>
#include <pbl/util/bits.h>
#include <pbl/util/size.h>

#include <devicetree/idle_states.h>
#include <devicetree/regs.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>

#define RCC(reg) (PBL_DT_REG_RCC + STM32U5_RCC_##reg##_OFFSET)
#define PWR(reg) (PBL_DT_REG_PWR + STM32U5_PWR_##reg##_OFFSET)

/*
 * Stop 2: every clock but the LSE and the LSI stops, the SRAMs and the registers are kept. The
 * system timer, LPTIM4, counts the LSE on in it, as LPTIM1 and LPTIM3 clock the display's VCOM,
 * all three in the SmartRun domain with their autonomous mode on (the clock driver sets it); the
 * EXTI lines of the buttons and the PMIC wake the CPU up, as does the timer. Peripherals that do
 * not work in Stop block it while busy: the USB while powered, the I2C buses during a transfer, the
 * eMMC, a USART receiving or sending.
 */

#ifdef CONFIG_TIMER_STM32_LPTIM

// MSIS, HSI16 and PLL1 start within tens of microseconds, the HSE within a few milliseconds
#define OSC_TIMEOUT_US 100000U
#define VOS_TIMEOUT_US 10000U

// What Stop turns off that ran before: the oscillators other than MSIS, and PLL1
#define RCC_CR_STOP_OFF \
  (RCC_CR_MSIKON | RCC_CR_HSION | RCC_CR_HSI48ON | RCC_CR_HSEON | RCC_CR_PLL1ON)

static void prv_on(uint32_t on, uint32_t rdy) {
  pbl_modifyl(0U, on, RCC(CR));
  (void)PBL_POLL_BUSY_US((pbl_readl(RCC(CR)) & rdy) == rdy, 1U, OSC_TIMEOUT_US);
}

/*
 * The CPU leaves Stop on MSIS, limited to 24 MHz, with the core supply in its Stop range and the
 * EPOD booster off: the supply comes back first, then MSIS at its range, as it feeds PLL1, then
 * the oscillators and PLL1 that ran, then the system clock. The flash wait states, the dividers
 * and the PLL1 configuration are kept.
 */
static void prv_clocks_restore(uint32_t cr, uint32_t sw, uint32_t msis, uint32_t vosr) {
  uint32_t rdy = 0U;

  pbl_modifyl(PWR_VOSR_VOS_MASK | PWR_VOSR_BOOSTEN, vosr, PWR(VOSR));
  (void)PBL_POLL_BUSY_US((pbl_readl(PWR(VOSR)) & PWR_VOSR_VOSRDY) != 0U, 1U, VOS_TIMEOUT_US);
  if ((vosr & PWR_VOSR_BOOSTEN) != 0U) {
    (void)PBL_POLL_BUSY_US((pbl_readl(PWR(VOSR)) & PWR_VOSR_BOOSTRDY) != 0U, 1U, VOS_TIMEOUT_US);
  }

  if (PBL_FIELD_GET(RCC_ICSCR1_MSISRANGE_MASK, pbl_readl(RCC(ICSCR1))) != msis) {
    (void)PBL_POLL_BUSY_US((pbl_readl(RCC(CR)) & RCC_CR_MSISRDY) != 0U, 1U, OSC_TIMEOUT_US);
    pbl_modifyl(RCC_ICSCR1_MSISRANGE_MASK, PBL_FIELD_PREP(RCC_ICSCR1_MSISRANGE_MASK, msis),
                RCC(ICSCR1));
    (void)PBL_POLL_BUSY_US((pbl_readl(RCC(CR)) & RCC_CR_MSISRDY) != 0U, 1U, OSC_TIMEOUT_US);
  }

  if ((cr & RCC_CR_MSIKON) != 0U) {
    rdy |= RCC_CR_MSIKRDY;
  }
  if ((cr & RCC_CR_HSION) != 0U) {
    rdy |= RCC_CR_HSIRDY;
  }
  if ((cr & RCC_CR_HSI48ON) != 0U) {
    rdy |= RCC_CR_HSI48RDY;
  }
  if ((cr & RCC_CR_HSEON) != 0U) {
    rdy |= RCC_CR_HSERDY;
  }
  prv_on(cr & (RCC_CR_MSIKON | RCC_CR_HSION | RCC_CR_HSI48ON | RCC_CR_HSEON), rdy);

  if ((cr & RCC_CR_PLL1ON) != 0U) {
    prv_on(RCC_CR_PLL1ON, RCC_CR_PLL1RDY);
  }

  if (PBL_FIELD_GET(RCC_CFGR1_SWS_MASK, pbl_readl(RCC(CFGR1))) != sw) {
    pbl_modifyl(RCC_CFGR1_SW_MASK, PBL_FIELD_PREP(RCC_CFGR1_SW_MASK, sw), RCC(CFGR1));
    (void)PBL_POLL_BUSY_US(PBL_FIELD_GET(RCC_CFGR1_SWS_MASK, pbl_readl(RCC(CFGR1))) == sw, 1U,
                           OSC_TIMEOUT_US);
  }
}

static void prv_enter_stop(void) {
  uint32_t cr = pbl_readl(RCC(CR)) & RCC_CR_STOP_OFF;
  uint32_t sw = PBL_FIELD_GET(RCC_CFGR1_SWS_MASK, pbl_readl(RCC(CFGR1)));
  uint32_t msis = PBL_FIELD_GET(RCC_ICSCR1_MSISRANGE_MASK, pbl_readl(RCC(ICSCR1)));
  uint32_t vosr = pbl_readl(PWR(VOSR)) & (PWR_VOSR_VOS_MASK | PWR_VOSR_BOOSTEN);

  // Woken up on MSIS, which runs PLL1, rather than on HSI16
  pbl_modifyl(RCC_CFGR1_STOPWUCK | RCC_CFGR1_STOPKERWUCK, 0U, RCC(CFGR1));
  pbl_modifyl(PWR_CR1_LPMS_MASK, PBL_FIELD_PREP(PWR_CR1_LPMS_MASK, PWR_CR1_LPMS_STOP2), PWR(CR1));

  pbl_cpu_idle_deep();

  prv_clocks_restore(cr, sw, msis, vosr);
  pbl_writel(PWR_SR_CSSF, PWR(SR));
}

#endif

const struct pbl_idle_state pbl_soc_idle_states[] = {
  [SOC_STM32_IDLE_WFI] = {.name = "wfi", .min_ticks = 1, .exit_ticks = 0, .enter = pbl_cpu_idle},
#ifdef CONFIG_TIMER_STM32_LPTIM
  [SOC_STM32_IDLE_STOP] = {
    .name = "stop",
    .min_ticks = PBL_US_TO_TICKS_CEIL(PBL_DT_IDLE_STATE_STOP_MIN_RESIDENCY_US),
    .exit_ticks = PBL_US_TO_TICKS_CEIL(PBL_DT_IDLE_STATE_STOP_EXIT_LATENCY_US),
    .enter = prv_enter_stop,
  },
#endif
};
const size_t pbl_soc_num_idle_states = PBL_ARRAY_SIZE(pbl_soc_idle_states);
