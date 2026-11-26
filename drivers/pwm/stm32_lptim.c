/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/pwm/stm32_lptim.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/mmio.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/units.h>

#include <hardware/stm32_lptim.h>

// A few cycles of the counter clock, for a write to reach the counter domain
#define SYNC_TIMEOUT_US 1000U

#define NUM_CHANNELS 2U

static const struct pbl_pwm_stm32_lptim *prv_lptim(const struct pbl_pwm_controller *ctrl) {
  return PBL_CONTAINER_OF(ctrl, const struct pbl_pwm_stm32_lptim, ctrl);
}

static inline uint32_t prv_getreg(const struct pbl_pwm_stm32_lptim *lptim, uint32_t offset) {
  return pbl_readl(lptim->base + offset);
}

static inline void prv_putreg(const struct pbl_pwm_stm32_lptim *lptim, uint32_t offset,
                              uint32_t value) {
  pbl_writel(value, lptim->base + offset);
}

// ARR and the compare values only take writes while the timer is enabled, and only one at a
// time reaches the counter domain
static int prv_write_sync(const struct pbl_pwm_stm32_lptim *lptim, uint32_t offset,
                          uint32_t value, uint32_t ok) {
  prv_putreg(lptim, STM32_LPTIM_ICR_OFFSET, ok);
  prv_putreg(lptim, offset, value);

  return PBL_POLL_BUSY_US((prv_getreg(lptim, STM32_LPTIM_ISR_OFFSET) & ok) != 0U, 1U,
                          SYNC_TIMEOUT_US);
}

static int prv_set_compare(const struct pbl_pwm_stm32_lptim *lptim, uint8_t channel,
                           uint32_t duty) {
  return (channel == 0U)
             ? prv_write_sync(lptim, STM32_LPTIM_CMP_OFFSET, duty, LPTIM_ISR_CMPOK)
             : prv_write_sync(lptim, STM32_LPTIM_CCR2_OFFSET, duty, LPTIM_ISR_CMP2OK);
}

static int prv_apply(const struct pbl_pwm_controller *ctrl, uint8_t channel,
                     const struct pbl_pwm_state *state) {
  const struct pbl_pwm_stm32_lptim *lptim = prv_lptim(ctrl);
  const uint32_t ch_mask =
      LPTIM_CCMR1_CCSEL(channel) | LPTIM_CCMR1_CCE(channel) | LPTIM_CCMR1_CCP(channel);
  uint32_t ccmr1 = prv_getreg(lptim, STM32_LPTIM_CCMR1_OFFSET);
  uint32_t cfg;
  uint64_t freq;
  uint32_t period;
  uint32_t duty;
  int ret;

  if (channel >= NUM_CHANNELS) {
    return -EINVAL;
  }

  if (!state->enabled) {
    ccmr1 &= ~ch_mask;
    prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, 0U);
    prv_putreg(lptim, STM32_LPTIM_CCMR1_OFFSET, ccmr1);
    if ((ccmr1 & (LPTIM_CCMR1_CCE(0U) | LPTIM_CCMR1_CCE(1U))) != 0U) {
      prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT);
    }
    return 0;
  }

  ret = pbl_clk_get_rate(&lptim->clk, &freq);
  if (ret != 0) {
    return ret;
  }

  period = (uint32_t)PBL_MIN(((uint64_t)state->period * freq) / PBL_NSEC_PER_SEC,
                             LPTIM_ARR_MASK + 1ULL);
  if (period < 2U) {
    return -EINVAL;
  }

  duty = (uint32_t)PBL_MIN(((uint64_t)state->duty_cycle * freq) / PBL_NSEC_PER_SEC, period);

  // In PWM mode the output toggles at the compare value and again at the end of the period: it
  // is low up to the compare value with CCP clear, and high with CCP set
  cfg = LPTIM_CCMR1_CCE(channel) |
        ((state->polarity == PBL_PWM_POLARITY_NORMAL)
             ? PBL_FIELD_PREP(LPTIM_CCMR1_CCP(channel), 1U)
             : 0U);

  if ((prv_getreg(lptim, STM32_LPTIM_CR_OFFSET) & LPTIM_CR_ENABLE) != 0U &&
      (ccmr1 & ch_mask) == cfg && prv_getreg(lptim, STM32_LPTIM_ARR_OFFSET) == period - 1U) {
    return prv_set_compare(lptim, channel, duty);
  }

  // The channel configuration only takes writes while the timer is disabled, which stops the
  // other channel too until it starts again from 0
  prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, 0U);
  prv_putreg(lptim, STM32_LPTIM_CFGR_OFFSET, 0U);
  prv_putreg(lptim, STM32_LPTIM_CCMR1_OFFSET, (ccmr1 & ~ch_mask) | cfg);
  prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, LPTIM_CR_ENABLE);

  ret = prv_write_sync(lptim, STM32_LPTIM_ARR_OFFSET, period - 1U, LPTIM_ISR_ARROK);
  if (ret == 0) {
    ret = prv_set_compare(lptim, channel, duty);
  }
  if (ret != 0) {
    prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, 0U);
    return ret;
  }

  prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT);

  return 0;
}

int pbl_pwm_stm32_lptim_init(const struct pbl_device *dev) {
  const struct pbl_pwm_stm32_lptim *lptim =
      PBL_CONTAINER_OF(dev, const struct pbl_pwm_stm32_lptim, ctrl.dev);
  int ret;

  ret = pbl_device_init(&lptim->clk.ctrl->dev);
  if (ret == 0) {
    ret = pbl_clk_enable(&lptim->clk);
  }
  if (ret != 0) {
    return ret;
  }

  // Whatever a bootloader left running, as the UNA one does with the display's
  prv_putreg(lptim, STM32_LPTIM_CR_OFFSET, 0U);
  prv_putreg(lptim, STM32_LPTIM_CFGR_OFFSET, 0U);
  prv_putreg(lptim, STM32_LPTIM_CCMR1_OFFSET, 0U);

  return pbl_pinctrl_select_state(&lptim->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
}

const struct pbl_pwm_controller_ops pbl_pwm_stm32_lptim_ops = {
  .apply = prv_apply,
};

#include PBL_DT_INSTANCES(st_stm32_pwm_lp)
