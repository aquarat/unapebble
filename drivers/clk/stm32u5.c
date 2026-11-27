/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/clk/stm32u5.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/mmio.h>
#include <pbl/kernel/time.h>
#include <pbl/util/bits.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#include <devicetree/regs.h>
#include <hardware/stm32u5_flash.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>

// The system clock goes to PLL1 R once PLL1 starts
static bool s_sys_on_pll1;

#define MHZ_PER_HZ 1000ULL
#define MHZ(n)     ((uint64_t)(n) * 1000000ULL * MHZ_PER_HZ)

// The core supply and the booster settle within tens of microseconds
#define VOS_TIMEOUT_US 10000U
// The HSE crystal starts within a few milliseconds, MSI and PLL1 within tens of microseconds
#define HSERDY_TIMEOUT_US 100000U
#define OSCRDY_TIMEOUT_US 10000U
// The LSE crystal starts within 2 s
#define LSERDY_TIMEOUT_US 2000000U

// Maximum of every clock in range 1
#define SYS_MAX_MHZ MHZ(160)

// Range 1 takes a flash wait state per 32 MHz of HCLK
#define FLASH_WS_MHZ MHZ(32)

// PLL1 reference and VCO limits
#define PLL_REF_MIN_MHZ MHZ(4)
#define PLL_REF_MAX_MHZ MHZ(16)
#define PLL_VCO_MIN_MHZ MHZ(128)
#define PLL_VCO_MAX_MHZ MHZ(544)
#define PLL_DIVM_MAX    16U
#define PLL_DIVN_MIN    4U
#define PLL_DIVN_MAX    512U
#define PLL_DIVPQR_MAX  128U
// The EPOD booster takes the PLL1 source divided down to 16 MHz at most
#define BOOST_MAX_MHZ MHZ(16)

#define NONE 0xffU

// An enable bit of a peripheral clock, with its parent, or its kernel clock multiplexer
struct gate {
  uint8_t reg;
  uint8_t bit;
  uint8_t parent;
  uint8_t mux;
  // Bit in SRDAMR of a peripheral of the SmartRun domain, which keeps its clocks in Stop 2
  uint8_t amen;
};

// A kernel clock multiplexer: the parent of each value of a field
struct mux {
  uint8_t reg;
  uint32_t mask;
  uint8_t parents[8];
};

enum {
  MUX_NONE,
  MUX_USART1,
  MUX_USART2,
  MUX_USART3,
  MUX_LPUART1,
  MUX_I2C1,
  MUX_I2C2,
  MUX_I2C3,
  MUX_I2C4,
  MUX_SPI1,
  MUX_SPI2,
  MUX_SPI3,
  MUX_LPTIM1,
  MUX_LPTIM2,
  MUX_LPTIM34,
  MUX_RNG,
  MUX_SDMMC,
  MUX_ICLK,
  MUX_USBPHY,
  NUM_MUXES,
};

#define MUX(_reg, _mask, ...)                                                \
  {                                                                          \
    .reg = STM32U5_RCC_##_reg##_OFFSET, .mask = RCC_##_reg##_##_mask##_MASK, \
    .parents = {__VA_ARGS__}                                                 \
  }

#define C(name) STM32U5_CLK_##name

// Unlisted values select nothing the driver supports, as PLL2 and PLL3 or HSI48 / 2
static const struct mux s_muxes[NUM_MUXES] = {
  [MUX_USART1] = MUX(CCIPR1, USART1SEL, C(PCLK2), C(SYSCLK), C(HSI16), C(LSE)),
  [MUX_USART2] = MUX(CCIPR1, USART2SEL, C(PCLK1), C(SYSCLK), C(HSI16), C(LSE)),
  [MUX_USART3] = MUX(CCIPR1, USART3SEL, C(PCLK1), C(SYSCLK), C(HSI16), C(LSE)),
  [MUX_LPUART1] =
      MUX(CCIPR3, LPUART1SEL, C(PCLK3), C(SYSCLK), C(HSI16), C(LSE), C(MSIK), NONE, NONE, NONE),
  [MUX_I2C1] = MUX(CCIPR1, I2C1SEL, C(PCLK1), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_I2C2] = MUX(CCIPR1, I2C2SEL, C(PCLK1), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_I2C3] = MUX(CCIPR3, I2C3SEL, C(PCLK3), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_I2C4] = MUX(CCIPR1, I2C4SEL, C(PCLK1), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_SPI1] = MUX(CCIPR1, SPI1SEL, C(PCLK2), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_SPI2] = MUX(CCIPR1, SPI2SEL, C(PCLK1), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_SPI3] = MUX(CCIPR3, SPI3SEL, C(PCLK3), C(SYSCLK), C(HSI16), C(MSIK)),
  [MUX_LPTIM1] = MUX(CCIPR3, LPTIM1SEL, C(MSIK), C(LSI), C(HSI16), C(LSE)),
  [MUX_LPTIM2] = MUX(CCIPR1, LPTIM2SEL, C(PCLK1), C(LSI), C(HSI16), C(LSE)),
  [MUX_LPTIM34] = MUX(CCIPR3, LPTIM34SEL, C(MSIK), C(LSI), C(HSI16), C(LSE)),
  [MUX_RNG] = MUX(CCIPR2, RNGSEL, C(HSI48), NONE, C(HSI16), NONE),
  [MUX_SDMMC] =
      {.reg = STM32U5_RCC_CCIPR2_OFFSET,
       .mask = RCC_CCIPR2_SDMMCSEL,
       .parents = {C(ICLK), C(PLL1_P)}},
  [MUX_ICLK] = MUX(CCIPR1, ICLKSEL, C(HSI48), NONE, C(PLL1_Q), C(MSIK)),
  // The USB HS PHY's reference; HSE / 2 and PLL1 P / 2 are not supported
  [MUX_USBPHY] = MUX(CCIPR2, USBPHYCSEL, C(HSE), NONE, C(PLL1_P), NONE),
};

#define GATE(_reg, _bit, _parent) \
  {.reg = STM32U5_RCC_##_reg##_OFFSET, .bit = (_bit), .parent = C(_parent), .amen = NONE}
#define GATE_MUX(_reg, _bit, _mux) \
  {.reg = STM32U5_RCC_##_reg##_OFFSET, .bit = (_bit), .mux = MUX_##_mux, .amen = NONE}
#define GATE_MUX_AM(_reg, _bit, _mux, _amen) \
  {.reg = STM32U5_RCC_##_reg##_OFFSET, .bit = (_bit), .mux = MUX_##_mux, .amen = (_amen)}

static const struct gate s_gates[PBL_CLK_STM32U5_NUM_CLKS] = {
  [C(GPIOA)] = GATE(AHB2ENR1, 0, HCLK),
  [C(GPIOB)] = GATE(AHB2ENR1, 1, HCLK),
  [C(GPIOC)] = GATE(AHB2ENR1, 2, HCLK),
  [C(GPIOD)] = GATE(AHB2ENR1, 3, HCLK),
  [C(GPIOE)] = GATE(AHB2ENR1, 4, HCLK),
  [C(GPIOF)] = GATE(AHB2ENR1, 5, HCLK),
  [C(GPIOG)] = GATE(AHB2ENR1, 6, HCLK),
  [C(GPIOH)] = GATE(AHB2ENR1, 7, HCLK),
  [C(GPIOI)] = GATE(AHB2ENR1, 8, HCLK),
  [C(GPDMA1)] = GATE(AHB1ENR, 0, HCLK),
  [C(FLASH)] = GATE(AHB1ENR, 8, HCLK),
  [C(CRC)] = GATE(AHB1ENR, 12, HCLK),
  [C(RNG)] = GATE_MUX(AHB2ENR1, 18, RNG),
  [C(OTG)] = GATE(AHB2ENR1, 14, HCLK),
  [C(USBPHY)] = GATE_MUX(AHB2ENR1, 15, USBPHY),
  [C(SDMMC1)] = GATE_MUX(AHB2ENR1, 27, SDMMC),
  [C(PWR)] = GATE(AHB3ENR, 2, HCLK),
  [C(USART1)] = GATE_MUX(APB2ENR, 14, USART1),
  [C(USART2)] = GATE_MUX(APB1ENR1, 17, USART2),
  [C(USART3)] = GATE_MUX(APB1ENR1, 18, USART3),
  [C(LPUART1)] = GATE_MUX(APB3ENR, 6, LPUART1),
  [C(I2C1)] = GATE_MUX(APB1ENR1, 21, I2C1),
  [C(I2C2)] = GATE_MUX(APB1ENR1, 22, I2C2),
  [C(I2C3)] = GATE_MUX(APB3ENR, 7, I2C3),
  [C(I2C4)] = GATE_MUX(APB1ENR2, 1, I2C4),
  [C(SPI1)] = GATE_MUX(APB2ENR, 12, SPI1),
  [C(SPI2)] = GATE_MUX(APB1ENR1, 14, SPI2),
  [C(SPI3)] = GATE_MUX(APB3ENR, 5, SPI3),
  [C(LPTIM1)] = GATE_MUX_AM(APB3ENR, 11, LPTIM1, 11),
  [C(LPTIM2)] = GATE_MUX(APB1ENR2, 5, LPTIM2),
  [C(LPTIM3)] = GATE_MUX_AM(APB3ENR, 12, LPTIM34, 12),
  [C(LPTIM4)] = GATE_MUX_AM(APB3ENR, 13, LPTIM34, 13),
  [C(RTCAPB)] = {.reg = STM32U5_RCC_APB3ENR_OFFSET, .bit = 21, .parent = C(PCLK3), .amen = 21},
  [C(SYSCFG)] = GATE(APB3ENR, 1, PCLK3),
  [C(WWDG)] = GATE(APB1ENR1, 11, PCLK1),
};

static const struct pbl_clk_stm32u5 *prv_rcc(const struct pbl_clk_controller *ctrl) {
  return PBL_CONTAINER_OF(ctrl, const struct pbl_clk_stm32u5, ctrl);
}

static inline uint32_t prv_getreg(const struct pbl_clk_stm32u5 *rcc, uint32_t offset) {
  return pbl_readl(rcc->base + offset);
}

static inline void prv_modify(const struct pbl_clk_stm32u5 *rcc, uint32_t offset, uint32_t mask,
                              uint32_t value) {
  pbl_modifyl(mask, PBL_FIELD_PREP(mask, value), rcc->base + offset);
}

static bool prv_is_gate(uint32_t id) {
  return id < PBL_ARRAY_SIZE(s_gates) && s_gates[id].reg != 0U;
}

// HPRE: 0xxx divides by 1, 1000 to 1111 by 2 to 512, skipping 32
static uint32_t prv_ahb_div(uint32_t pre) {
  if ((pre & RCC_AHB_PRE_DIVIDED) == 0U) {
    return 1U;
  }

  pre &= ~RCC_AHB_PRE_DIVIDED;

  return 1U << (pre + (pre >= 4U ? 2U : 1U));
}

// PPRE: 0xx divides by 1, 100 to 111 by 2 to 16
static uint32_t prv_apb_div(uint32_t pre) {
  if ((pre & RCC_APB_PRE_DIVIDED) == 0U) {
    return 1U;
  }

  return 1U << ((pre & ~RCC_APB_PRE_DIVIDED) + 1U);
}

static int prv_ahb_pre(uint32_t div) {
  static const uint16_t s_divs[] = {2, 4, 8, 16, 64, 128, 256, 512};

  if (div == 1U) {
    return 0;
  }
  for (uint32_t i = 0U; i < PBL_ARRAY_SIZE(s_divs); i++) {
    if (s_divs[i] == div) {
      return (int)(RCC_AHB_PRE_DIVIDED | i);
    }
  }

  return -EINVAL;
}

static int prv_apb_pre(uint32_t div) {
  for (uint32_t i = 0U; i < 4U; i++) {
    if (div == (2U << i)) {
      return (int)(RCC_APB_PRE_DIVIDED | i);
    }
  }

  return div == 1U ? 0 : -EINVAL;
}

static const uint32_t s_msi_ranges_hz[] = STM32U5_MSI_RANGES_HZ;

// MSIS and MSIK take their range from ICSCR1, which the init selects
static uint32_t prv_msi_range(const struct pbl_clk_stm32u5 *rcc, uint32_t mask) {
  return PBL_FIELD_GET(mask, prv_getreg(rcc, STM32U5_RCC_ICSCR1_OFFSET));
}

static uint32_t prv_sws(const struct pbl_clk_stm32u5 *rcc) {
  return PBL_FIELD_GET(RCC_CFGR1_SWS_MASK, prv_getreg(rcc, STM32U5_RCC_CFGR1_OFFSET));
}

static bool prv_pll1_on(const struct pbl_clk_stm32u5 *rcc) {
  return (prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & RCC_CR_PLL1ON) != 0U;
}

static int prv_hclk_rate(const struct pbl_clk_controller *ctrl, uint64_t *rate_mhz) {
  return pbl_clk_get_rate(&(struct pbl_clk)PBL_CLK(ctrl, C(HCLK)), rate_mhz);
}

static void prv_update_core_clock(const struct pbl_clk_controller *ctrl) {
  uint64_t rate_mhz;

  if (prv_hclk_rate(ctrl, &rate_mhz) == 0) {
    pbl_cycles_per_sec_set((uint32_t)(rate_mhz / MHZ_PER_HZ));
  }
}

// The wait states HCLK needs
static uint32_t prv_flash_ws(uint64_t hclk_mhz) {
  return hclk_mhz == 0U ? 0U : (uint32_t)((hclk_mhz - 1U) / FLASH_WS_MHZ);
}

static void prv_flash_ws_set(uint32_t ws) {
  uintptr_t acr = PBL_DT_REG_FLASH + STM32U5_FLASH_ACR_OFFSET;

  pbl_modifyl(FLASH_ACR_LATENCY_MASK, PBL_FIELD_PREP(FLASH_ACR_LATENCY_MASK, ws), acr);
  while (PBL_FIELD_GET(FLASH_ACR_LATENCY_MASK, pbl_readl(acr)) != ws) {
  }
}

// Before HCLK changes: enough wait states for it at the faster of its rates
static void prv_flash_ws_raise(uint64_t hclk_mhz) {
  uint32_t ws = prv_flash_ws(hclk_mhz);

  if (ws > PBL_FIELD_GET(FLASH_ACR_LATENCY_MASK, pbl_readl(PBL_DT_REG_FLASH))) {
    prv_flash_ws_set(ws);
  }
}

// After HCLK changed: the wait states of its rate, and the CPU cycles busy waits count
static void prv_hclk_changed(const struct pbl_clk_controller *ctrl) {
  uint64_t rate_mhz;

  if (prv_hclk_rate(ctrl, &rate_mhz) == 0) {
    prv_flash_ws_set(prv_flash_ws(rate_mhz));
  }
  prv_update_core_clock(ctrl);
}

// HCLK once the system clock runs at a rate, with the AHB divider it has
static uint64_t prv_hclk_at(const struct pbl_clk_stm32u5 *rcc, uint64_t sys_mhz) {
  return sys_mhz /
         prv_ahb_div(PBL_FIELD_GET(RCC_CFGR2_HPRE_MASK, prv_getreg(rcc, STM32U5_RCC_CFGR2_OFFSET)));
}

// Turns an oscillator on, at bit on of a register, and waits for its ready bit
static int prv_osc_on(const struct pbl_clk_stm32u5 *rcc, uint32_t offset, uint32_t on, uint32_t rdy,
                      uint32_t timeout_us) {
  pbl_modifyl(0U, on, rcc->base + offset);

  return PBL_POLL_BUSY_US((prv_getreg(rcc, offset) & rdy) != 0U, 1U, timeout_us);
}

// The backup domain takes writes once PWR lets it; the LSE, the LSI and the RTC clock are in it
static void prv_bdcr_unlock(const struct pbl_clk_stm32u5 *rcc) {
  pbl_modifyl(0U, PWR_DBPR_DBP, rcc->pwr + STM32U5_PWR_DBPR_OFFSET);
}

// The LSE keeps running across resets, as the RTC counts it: it is only set up when it does not
// run. The peripherals other than the RTC take it once LSESYS is on too.
static int prv_lse_on(const struct pbl_clk_stm32u5 *rcc) {
  uintptr_t bdcr = rcc->base + STM32U5_RCC_BDCR_OFFSET;
  int ret;

  prv_bdcr_unlock(rcc);
  if ((pbl_readl(bdcr) & RCC_BDCR_LSERDY) == 0U) {
    pbl_modifyl(RCC_BDCR_LSEON, 0U, bdcr);
    pbl_modifyl(RCC_BDCR_LSEBYP, rcc->lse_bypass ? RCC_BDCR_LSEBYP : 0U, bdcr);
    pbl_modifyl(0U, RCC_BDCR_LSEON, bdcr);
    ret = PBL_POLL_BUSY_US((pbl_readl(bdcr) & RCC_BDCR_LSERDY) != 0U, 100U, LSERDY_TIMEOUT_US);
    if (ret != 0) {
      return ret;
    }
  }

  pbl_modifyl(0U, RCC_BDCR_LSESYSEN, bdcr);

  return PBL_POLL_BUSY_US((pbl_readl(bdcr) & RCC_BDCR_LSESYSRDY) != 0U, 10U, OSCRDY_TIMEOUT_US);
}

static int prv_enable(const struct pbl_clk_controller *ctrl, uint32_t id) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);

  switch (id) {
    case C(HSE):
      return prv_osc_on(rcc, STM32U5_RCC_CR_OFFSET, RCC_CR_HSEON, RCC_CR_HSERDY, HSERDY_TIMEOUT_US);
    case C(HSI16):
      return prv_osc_on(rcc, STM32U5_RCC_CR_OFFSET, RCC_CR_HSION, RCC_CR_HSIRDY, OSCRDY_TIMEOUT_US);
    case C(HSI48):
      return prv_osc_on(rcc, STM32U5_RCC_CR_OFFSET, RCC_CR_HSI48ON, RCC_CR_HSI48RDY,
                        OSCRDY_TIMEOUT_US);
    case C(MSIK):
      return prv_osc_on(rcc, STM32U5_RCC_CR_OFFSET, RCC_CR_MSIKON, RCC_CR_MSIKRDY,
                        OSCRDY_TIMEOUT_US);
    case C(LSI):
      prv_bdcr_unlock(rcc);
      return prv_osc_on(rcc, STM32U5_RCC_BDCR_OFFSET, RCC_BDCR_LSION, RCC_BDCR_LSIRDY,
                        OSCRDY_TIMEOUT_US);
    case C(LSE):
      return prv_lse_on(rcc);
    case C(RTC):
      prv_bdcr_unlock(rcc);
      pbl_modifyl(0U, RCC_BDCR_RTCEN, rcc->base + STM32U5_RCC_BDCR_OFFSET);
      return 0;
    default:
      break;
  }

  if (!prv_is_gate(id)) {
    return 0;
  }

  if (s_gates[id].amen != NONE) {
    pbl_modifyl(0U, PBL_BIT(s_gates[id].amen), rcc->base + STM32U5_RCC_SRDAMR_OFFSET);
  }
  pbl_modifyl(0U, PBL_BIT(s_gates[id].bit), rcc->base + s_gates[id].reg);
  // The clock runs two cycles after the write; the read back waits for them
  (void)prv_getreg(rcc, s_gates[id].reg);

  return 0;
}

// The oscillators running the system clock, or PLL1, stay on whatever is asked
static int prv_disable(const struct pbl_clk_controller *ctrl, uint32_t id) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);

  switch (id) {
    case C(HSE):
      pbl_modifyl(RCC_CR_HSEON, 0U, rcc->base + STM32U5_RCC_CR_OFFSET);
      return 0;
    case C(HSI16):
      pbl_modifyl(RCC_CR_HSION, 0U, rcc->base + STM32U5_RCC_CR_OFFSET);
      return 0;
    case C(HSI48):
      pbl_modifyl(RCC_CR_HSI48ON, 0U, rcc->base + STM32U5_RCC_CR_OFFSET);
      return 0;
    case C(MSIK):
      pbl_modifyl(RCC_CR_MSIKON, 0U, rcc->base + STM32U5_RCC_CR_OFFSET);
      return 0;
    case C(LSI):
      prv_bdcr_unlock(rcc);
      pbl_modifyl(RCC_BDCR_LSION, 0U, rcc->base + STM32U5_RCC_BDCR_OFFSET);
      return 0;
    case C(RTC):
      prv_bdcr_unlock(rcc);
      pbl_modifyl(RCC_BDCR_RTCEN, 0U, rcc->base + STM32U5_RCC_BDCR_OFFSET);
      return 0;
    default:
      break;
  }

  if (prv_is_gate(id)) {
    pbl_modifyl(PBL_BIT(s_gates[id].bit), 0U, rcc->base + s_gates[id].reg);
    if (s_gates[id].amen != NONE) {
      pbl_modifyl(PBL_BIT(s_gates[id].amen), 0U, rcc->base + STM32U5_RCC_SRDAMR_OFFSET);
    }
  }

  return 0;
}

static int prv_mux_parent(const struct pbl_clk_controller *ctrl, const struct mux *mux,
                          struct pbl_clk *parent) {
  uint32_t sel = PBL_FIELD_GET(mux->mask, prv_getreg(prv_rcc(ctrl), mux->reg));

  if (sel >= PBL_ARRAY_SIZE(mux->parents) || mux->parents[sel] == NONE ||
      (sel != 0U && mux->parents[sel] == 0U)) {
    return -ENOTSUP;
  }

  *parent = (struct pbl_clk)PBL_CLK(ctrl, mux->parents[sel]);

  return 0;
}

static int prv_get_parent(const struct pbl_clk_controller *ctrl, uint32_t id,
                          struct pbl_clk *parent) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);

  switch (id) {
    case C(HSE):
      *parent = rcc->hse;
      return 0;
    case C(LSE):
      *parent = rcc->lse;
      return 0;
    case C(HSI16):
    case C(HSI48):
    case C(MSIS):
    case C(MSIK):
    case C(LSI):
      return -ENOENT;
    case C(PLL1_SRC):
      switch (PBL_FIELD_GET(RCC_PLL1CFGR_SRC_MASK, prv_getreg(rcc, STM32U5_RCC_PLL1CFGR_OFFSET))) {
        case RCC_PLL1CFGR_SRC_MSIS:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(MSIS));
          return 0;
        case RCC_PLL1CFGR_SRC_HSI16:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(HSI16));
          return 0;
        case RCC_PLL1CFGR_SRC_HSE:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(HSE));
          return 0;
        default:
          return -ENOENT;
      }
    case C(PLL1_P):
    case C(PLL1_Q):
    case C(PLL1_R):
      *parent = (struct pbl_clk)PBL_CLK(ctrl, C(PLL1_SRC));
      return 0;
    case C(SYSCLK):
      switch (prv_sws(rcc)) {
        case RCC_CFGR1_SW_MSIS:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(MSIS));
          return 0;
        case RCC_CFGR1_SW_HSI16:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(HSI16));
          return 0;
        case RCC_CFGR1_SW_HSE:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(HSE));
          return 0;
        default:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(PLL1_R));
          return 0;
      }
    case C(HCLK):
      *parent = (struct pbl_clk)PBL_CLK(ctrl, C(SYSCLK));
      return 0;
    case C(PCLK1):
    case C(PCLK2):
    case C(PCLK3):
      *parent = (struct pbl_clk)PBL_CLK(ctrl, C(HCLK));
      return 0;
    case C(RTC):
      switch (PBL_FIELD_GET(RCC_BDCR_RTCSEL_MASK, prv_getreg(rcc, STM32U5_RCC_BDCR_OFFSET))) {
        case RCC_BDCR_RTCSEL_LSE:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(LSE));
          return 0;
        case RCC_BDCR_RTCSEL_LSI:
          *parent = (struct pbl_clk)PBL_CLK(ctrl, C(LSI));
          return 0;
        default:
          return -ENOENT;
      }
    case C(ICLK):
      return prv_mux_parent(ctrl, &s_muxes[MUX_ICLK], parent);
    default:
      if (!prv_is_gate(id)) {
        return -EINVAL;
      }
      if (s_gates[id].mux != MUX_NONE) {
        return prv_mux_parent(ctrl, &s_muxes[s_gates[id].mux], parent);
      }
      *parent = (struct pbl_clk)PBL_CLK(ctrl, s_gates[id].parent);
      return 0;
  }
}

static uint32_t prv_pll1_div(uint32_t id, uint32_t divr) {
  switch (id) {
    case C(PLL1_P):
      return PBL_FIELD_GET(RCC_PLL1DIVR_P_MASK, divr) + 1U;
    case C(PLL1_Q):
      return PBL_FIELD_GET(RCC_PLL1DIVR_Q_MASK, divr) + 1U;
    default:
      return PBL_FIELD_GET(RCC_PLL1DIVR_R_MASK, divr) + 1U;
  }
}

static int prv_get_rate(const struct pbl_clk_controller *ctrl, uint32_t id, uint64_t parent_mhz,
                        uint64_t *rate_mhz) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uint32_t cfgr2 = prv_getreg(rcc, STM32U5_RCC_CFGR2_OFFSET);

  switch (id) {
    case C(HSI16):
      *rate_mhz = STM32U5_HSI16_HZ * MHZ_PER_HZ;
      return 0;
    case C(HSI48):
      *rate_mhz = STM32U5_HSI48_HZ * MHZ_PER_HZ;
      return 0;
    case C(MSIS):
      *rate_mhz = s_msi_ranges_hz[prv_msi_range(rcc, RCC_ICSCR1_MSISRANGE_MASK)] * MHZ_PER_HZ;
      return 0;
    case C(MSIK):
      *rate_mhz = s_msi_ranges_hz[prv_msi_range(rcc, RCC_ICSCR1_MSIKRANGE_MASK)] * MHZ_PER_HZ;
      return 0;
    case C(LSI):
      *rate_mhz = STM32U5_LSI_HZ * MHZ_PER_HZ;
      if ((prv_getreg(rcc, STM32U5_RCC_BDCR_OFFSET) & RCC_BDCR_LSIPREDIV) != 0U) {
        *rate_mhz /= 128U;
      }
      return 0;
    case C(PLL1_P):
    case C(PLL1_Q):
    case C(PLL1_R): {
      uint32_t cfgr = prv_getreg(rcc, STM32U5_RCC_PLL1CFGR_OFFSET);
      uint32_t divr = prv_getreg(rcc, STM32U5_RCC_PLL1DIVR_OFFSET);
      uint32_t divm = PBL_FIELD_GET(RCC_PLL1CFGR_M_MASK, cfgr) + 1U;

      if ((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & RCC_CR_PLL1RDY) == 0U) {
        *rate_mhz = 0U;
        return 0;
      }
      *rate_mhz = parent_mhz / divm * (PBL_FIELD_GET(RCC_PLL1DIVR_N_MASK, divr) + 1U) /
                  prv_pll1_div(id, divr);
      return 0;
    }
    case C(HCLK):
      *rate_mhz = parent_mhz / prv_ahb_div(PBL_FIELD_GET(RCC_CFGR2_HPRE_MASK, cfgr2));
      return 0;
    case C(PCLK1):
      *rate_mhz = parent_mhz / prv_apb_div(PBL_FIELD_GET(RCC_CFGR2_PPRE1_MASK, cfgr2));
      return 0;
    case C(PCLK2):
      *rate_mhz = parent_mhz / prv_apb_div(PBL_FIELD_GET(RCC_CFGR2_PPRE2_MASK, cfgr2));
      return 0;
    case C(PCLK3):
      *rate_mhz =
          parent_mhz / prv_apb_div(PBL_FIELD_GET(RCC_CFGR3_PPRE3_MASK,
                                                 prv_getreg(rcc, STM32U5_RCC_CFGR3_OFFSET)));
      return 0;
    default:
      *rate_mhz = parent_mhz;
      return 0;
  }
}

static int prv_pll1_off(const struct pbl_clk_stm32u5 *rcc) {
  if (prv_sws(rcc) == RCC_CFGR1_SW_PLL1) {
    return -EBUSY;
  }

  pbl_modifyl(RCC_CR_PLL1ON, 0U, rcc->base + STM32U5_RCC_CR_OFFSET);

  return PBL_POLL_BUSY_US((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & RCC_CR_PLL1RDY) == 0U, 1U,
                          OSCRDY_TIMEOUT_US);
}

static int prv_set_pll1_src(const struct pbl_clk_controller *ctrl, const struct pbl_clk *parent) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uint32_t sel;
  int ret;

  if (parent->ctrl != ctrl) {
    return -EINVAL;
  }

  switch (parent->id) {
    case C(MSIS):
      sel = RCC_PLL1CFGR_SRC_MSIS;
      break;
    case C(HSI16):
      sel = RCC_PLL1CFGR_SRC_HSI16;
      break;
    case C(HSE):
      sel = RCC_PLL1CFGR_SRC_HSE;
      break;
    default:
      return -EINVAL;
  }

  if (PBL_FIELD_GET(RCC_PLL1CFGR_SRC_MASK, prv_getreg(rcc, STM32U5_RCC_PLL1CFGR_OFFSET)) == sel) {
    return 0;
  }

  // PLL1 runs from it, so it changes only while PLL1 is off
  ret = prv_pll1_off(rcc);
  if (ret != 0) {
    return ret;
  }

  ret = prv_enable(ctrl, parent->id);
  if (ret != 0) {
    return ret;
  }

  prv_modify(rcc, STM32U5_RCC_PLL1CFGR_OFFSET, RCC_PLL1CFGR_SRC_MASK, sel);

  return 0;
}

static int prv_set_sys_parent(const struct pbl_clk_controller *ctrl, const struct pbl_clk *parent) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uintptr_t vosr = rcc->pwr + STM32U5_PWR_VOSR_OFFSET;
  uint64_t rate_mhz;
  uint64_t old_mhz;
  uint32_t sw;
  int ret;

  if (parent->ctrl != ctrl) {
    return -EINVAL;
  }

  switch (parent->id) {
    case C(MSIS):
      sw = RCC_CFGR1_SW_MSIS;
      break;
    case C(HSI16):
      sw = RCC_CFGR1_SW_HSI16;
      ret = prv_enable(ctrl, C(HSI16));
      if (ret != 0) {
        return ret;
      }
      break;
    case C(HSE):
      sw = RCC_CFGR1_SW_HSE;
      ret = prv_enable(ctrl, C(HSE));
      if (ret != 0) {
        return ret;
      }
      break;
    case C(PLL1_R):
      sw = RCC_CFGR1_SW_PLL1;
      // Assigned parents come before assigned rates: the rate of PLL1 R starts it, then switches
      if ((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & RCC_CR_PLL1RDY) == 0U) {
        s_sys_on_pll1 = true;
        return 0;
      }
      // Above 55 MHz the core supply needs the booster, which PLL1 clocks
      if ((pbl_readl(vosr) & PWR_VOSR_BOOSTEN) != 0U) {
        ret = PBL_POLL_BUSY_US((pbl_readl(vosr) & PWR_VOSR_BOOSTRDY) != 0U, 1U, VOS_TIMEOUT_US);
        if (ret != 0) {
          return ret;
        }
      }
      break;
    default:
      return -EINVAL;
  }

  ret = pbl_clk_get_rate(parent, &rate_mhz);
  if (ret != 0) {
    return ret;
  }
  if (rate_mhz == 0U || rate_mhz > SYS_MAX_MHZ) {
    return -EINVAL;
  }

  ret = prv_hclk_rate(ctrl, &old_mhz);
  if (ret != 0) {
    return ret;
  }

  prv_flash_ws_raise(PBL_MAX(old_mhz, prv_hclk_at(rcc, rate_mhz)));
  prv_modify(rcc, STM32U5_RCC_CFGR1_OFFSET, RCC_CFGR1_SW_MASK, sw);
  while (prv_sws(rcc) != sw) {
  }
  prv_hclk_changed(ctrl);

  return 0;
}

static int prv_set_mux_parent(const struct pbl_clk_controller *ctrl, const struct mux *mux,
                              const struct pbl_clk *parent) {
  if (parent->ctrl != ctrl) {
    return -EINVAL;
  }

  for (uint32_t sel = 0U; sel < PBL_ARRAY_SIZE(mux->parents); sel++) {
    if (mux->parents[sel] == parent->id && (sel == 0U || mux->parents[sel] != 0U)) {
      prv_modify(prv_rcc(ctrl), mux->reg, mux->mask, sel);
      return 0;
    }
  }

  return -EINVAL;
}

// The selection only changes after a reset of the backup domain, which would stop the LSE and
// lose the time: it is set once, from the first boot. Another one found, as another firmware
// left it, stays.
static int prv_set_rtc_parent(const struct pbl_clk_controller *ctrl, const struct pbl_clk *parent) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uint32_t cur = PBL_FIELD_GET(RCC_BDCR_RTCSEL_MASK, prv_getreg(rcc, STM32U5_RCC_BDCR_OFFSET));
  uint32_t sel;

  if (parent->ctrl != ctrl) {
    return -EINVAL;
  }

  switch (parent->id) {
    case C(LSE):
      sel = RCC_BDCR_RTCSEL_LSE;
      break;
    case C(LSI):
      sel = RCC_BDCR_RTCSEL_LSI;
      break;
    default:
      return -EINVAL;
  }

  if (cur != RCC_BDCR_RTCSEL_NONE) {
    return 0;
  }

  prv_bdcr_unlock(rcc);
  prv_modify(rcc, STM32U5_RCC_BDCR_OFFSET, RCC_BDCR_RTCSEL_MASK, sel);

  return 0;
}

static int prv_set_parent(const struct pbl_clk_controller *ctrl, uint32_t id,
                          const struct pbl_clk *parent) {
  switch (id) {
    case C(PLL1_SRC):
      return prv_set_pll1_src(ctrl, parent);
    case C(SYSCLK):
      return prv_set_sys_parent(ctrl, parent);
    case C(RTC):
      return prv_set_rtc_parent(ctrl, parent);
    case C(ICLK):
      return prv_set_mux_parent(ctrl, &s_muxes[MUX_ICLK], parent);
    default:
      if (!prv_is_gate(id) || s_gates[id].mux == MUX_NONE) {
        return -EINVAL;
      }
      return prv_set_mux_parent(ctrl, &s_muxes[s_gates[id].mux], parent);
  }
}

// MSIS changes range while ready, a faster one with the wait states raised first. It does not
// while it runs PLL1.
static int prv_set_msi_rate(const struct pbl_clk_controller *ctrl, uint32_t id, uint64_t rate_mhz) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  bool msis = id == C(MSIS);
  uint32_t mask = msis ? RCC_ICSCR1_MSISRANGE_MASK : RCC_ICSCR1_MSIKRANGE_MASK;
  uint32_t rdy = msis ? RCC_CR_MSISRDY : RCC_CR_MSIKRDY;
  bool sys = msis && prv_sws(rcc) == RCC_CFGR1_SW_MSIS;
  uint32_t range;
  uint64_t old_mhz;
  int ret;

  for (range = 0U; range < PBL_ARRAY_SIZE(s_msi_ranges_hz); range++) {
    if (s_msi_ranges_hz[range] * MHZ_PER_HZ == rate_mhz) {
      break;
    }
  }
  if (range == PBL_ARRAY_SIZE(s_msi_ranges_hz)) {
    return -EINVAL;
  }

  if (prv_msi_range(rcc, mask) == range) {
    return 0;
  }

  if (msis && prv_pll1_on(rcc) &&
      PBL_FIELD_GET(RCC_PLL1CFGR_SRC_MASK, prv_getreg(rcc, STM32U5_RCC_PLL1CFGR_OFFSET)) ==
          RCC_PLL1CFGR_SRC_MSIS) {
    return -EBUSY;
  }

  // Not while it starts, nor while it changes range
  if ((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & (msis ? RCC_CR_MSISON : RCC_CR_MSIKON)) != 0U) {
    ret = PBL_POLL_BUSY_US((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & rdy) != 0U, 1U,
                           OSCRDY_TIMEOUT_US);
    if (ret != 0) {
      return ret;
    }
  }

  if (sys) {
    ret = prv_hclk_rate(ctrl, &old_mhz);
    if (ret != 0) {
      return ret;
    }
    prv_flash_ws_raise(PBL_MAX(old_mhz, prv_hclk_at(rcc, rate_mhz)));
  }

  prv_modify(rcc, STM32U5_RCC_ICSCR1_OFFSET, mask, range);

  if ((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & (msis ? RCC_CR_MSISON : RCC_CR_MSIKON)) != 0U) {
    ret = PBL_POLL_BUSY_US((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & rdy) != 0U, 1U,
                           OSCRDY_TIMEOUT_US);
    if (ret != 0) {
      return ret;
    }
  }

  if (sys) {
    prv_hclk_changed(ctrl);
  }

  return 0;
}

static uint32_t prv_pll1_div_mask(uint32_t id) {
  switch (id) {
    case C(PLL1_P):
      return RCC_PLL1DIVR_P_MASK;
    case C(PLL1_Q):
      return RCC_PLL1DIVR_Q_MASK;
    default:
      return RCC_PLL1DIVR_R_MASK;
  }
}

static uint32_t prv_pll1_en(uint32_t id) {
  switch (id) {
    case C(PLL1_P):
      return RCC_PLL1CFGR_PEN;
    case C(PLL1_Q):
      return RCC_PLL1CFGR_QEN;
    default:
      return RCC_PLL1CFGR_REN;
  }
}

// The MBOOST field of the EPOD booster prescaler for a PLL1 source: 1, 2, then even divisions
static uint32_t prv_mboost(uint64_t src_mhz) {
  static const uint8_t s_divs[] = {1, 2, 4, 6, 8, 10, 12, 14, 16};

  for (uint32_t i = 0U; i < PBL_ARRAY_SIZE(s_divs); i++) {
    if (src_mhz <= BOOST_MAX_MHZ * s_divs[i]) {
      return i;
    }
  }

  return PBL_ARRAY_SIZE(s_divs) - 1U;
}

// Configures PLL1 for one of its outputs and starts it: with its VCO as it is when it gives the
// rate, otherwise with the first reference within limits. The other outputs keep their dividers.
// PLL1 must not run the system clock, but for the rate it already has.
static int prv_pll1_set_rate(const struct pbl_clk_controller *ctrl, uint32_t id, uint64_t rate_mhz,
                             uint64_t src_mhz) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uintptr_t vosr = rcc->pwr + STM32U5_PWR_VOSR_OFFSET;
  uint32_t cfgr = prv_getreg(rcc, STM32U5_RCC_PLL1CFGR_OFFSET);
  uint32_t divr = prv_getreg(rcc, STM32U5_RCC_PLL1DIVR_OFFSET);
  uint32_t m = PBL_FIELD_GET(RCC_PLL1CFGR_M_MASK, cfgr) + 1U;
  uint32_t n = PBL_FIELD_GET(RCC_PLL1DIVR_N_MASK, divr) + 1U;
  bool found = false;
  bool boost;
  uint32_t div = 0U;
  uint64_t ref_mhz;
  int ret;

  if (src_mhz == 0U || rate_mhz == 0U) {
    return -EINVAL;
  }

  // The VCO it has, when within limits
  ref_mhz = src_mhz / m;
  if (src_mhz % m == 0U && ref_mhz >= PLL_REF_MIN_MHZ && ref_mhz <= PLL_REF_MAX_MHZ &&
      (ref_mhz * n) % rate_mhz == 0U && ref_mhz * n >= PLL_VCO_MIN_MHZ &&
      ref_mhz * n <= PLL_VCO_MAX_MHZ) {
    div = (uint32_t)((ref_mhz * n) / rate_mhz);
    found = div >= 1U && div <= PLL_DIVPQR_MAX;
  }

  for (uint32_t d = 1U; !found && d <= PLL_DIVPQR_MAX; d++) {
    uint64_t vco_mhz = rate_mhz * d;

    if (vco_mhz < PLL_VCO_MIN_MHZ || vco_mhz > PLL_VCO_MAX_MHZ) {
      continue;
    }

    for (m = 1U; m <= PLL_DIVM_MAX; m++) {
      ref_mhz = src_mhz / m;
      if (src_mhz % m != 0U || ref_mhz < PLL_REF_MIN_MHZ || ref_mhz > PLL_REF_MAX_MHZ ||
          vco_mhz % ref_mhz != 0U) {
        continue;
      }
      n = (uint32_t)(vco_mhz / ref_mhz);
      if (n >= PLL_DIVN_MIN && n <= PLL_DIVN_MAX) {
        div = d;
        found = true;
        break;
      }
    }
  }

  if (!found) {
    return -EINVAL;
  }

  // Already so: nothing to restart
  if (prv_pll1_on(rcc) && (cfgr & prv_pll1_en(id)) != 0U &&
      PBL_FIELD_GET(RCC_PLL1CFGR_M_MASK, cfgr) + 1U == m &&
      PBL_FIELD_GET(RCC_PLL1DIVR_N_MASK, divr) + 1U == n && prv_pll1_div(id, divr) == div &&
      (cfgr & RCC_PLL1CFGR_FRACEN) == 0U) {
    return 0;
  }

  ret = prv_pll1_off(rcc);
  if (ret != 0) {
    return ret;
  }

  // The booster prescaler only changes with the booster off
  boost = (pbl_readl(vosr) & PWR_VOSR_BOOSTEN) != 0U;
  pbl_modifyl(PWR_VOSR_BOOSTEN, 0U, vosr);

  pbl_modifyl(
      RCC_PLL1CFGR_RGE_MASK | RCC_PLL1CFGR_FRACEN | RCC_PLL1CFGR_M_MASK | RCC_PLL1CFGR_MBOOST_MASK,
      PBL_FIELD_PREP(RCC_PLL1CFGR_RGE_MASK,
                     ref_mhz < MHZ(8) ? RCC_PLL1CFGR_RGE_4_8MHZ : RCC_PLL1CFGR_RGE_8_16MHZ) |
          PBL_FIELD_PREP(RCC_PLL1CFGR_M_MASK, m - 1U) |
          PBL_FIELD_PREP(RCC_PLL1CFGR_MBOOST_MASK, prv_mboost(src_mhz)) | prv_pll1_en(id),
      rcc->base + STM32U5_RCC_PLL1CFGR_OFFSET);
  pbl_modifyl(
      RCC_PLL1DIVR_N_MASK | prv_pll1_div_mask(id),
      PBL_FIELD_PREP(RCC_PLL1DIVR_N_MASK, n - 1U) | PBL_FIELD_PREP(prv_pll1_div_mask(id), div - 1U),
      rcc->base + STM32U5_RCC_PLL1DIVR_OFFSET);

  if (boost) {
    pbl_modifyl(0U, PWR_VOSR_BOOSTEN, vosr);
  }

  pbl_modifyl(0U, RCC_CR_PLL1ON, rcc->base + STM32U5_RCC_CR_OFFSET);

  return PBL_POLL_BUSY_US((prv_getreg(rcc, STM32U5_RCC_CR_OFFSET) & RCC_CR_PLL1RDY) != 0U, 1U,
                          OSCRDY_TIMEOUT_US);
}

static int prv_set_rate(const struct pbl_clk_controller *ctrl, uint32_t id, uint64_t rate_mhz,
                        uint64_t parent_mhz) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uint64_t old_mhz;
  uint32_t div;
  int pre;
  int ret;

  switch (id) {
    case C(MSIS):
    case C(MSIK):
      return prv_set_msi_rate(ctrl, id, rate_mhz);
    case C(PLL1_P):
    case C(PLL1_Q):
    case C(PLL1_R):
      ret = prv_pll1_set_rate(ctrl, id, rate_mhz, parent_mhz);
      if (ret == 0 && id == C(PLL1_R) && s_sys_on_pll1) {
        s_sys_on_pll1 = false;
        ret = prv_set_sys_parent(ctrl, &(struct pbl_clk)PBL_CLK(ctrl, C(PLL1_R)));
      }
      return ret;
    default:
      break;
  }

  if (rate_mhz == 0U || parent_mhz % rate_mhz != 0U) {
    return -EINVAL;
  }
  div = (uint32_t)(parent_mhz / rate_mhz);

  switch (id) {
    case C(HCLK):
      pre = prv_ahb_pre(div);
      if (pre < 0 || rate_mhz > SYS_MAX_MHZ) {
        return -EINVAL;
      }
      ret = prv_hclk_rate(ctrl, &old_mhz);
      if (ret != 0) {
        return ret;
      }
      prv_flash_ws_raise(PBL_MAX(old_mhz, rate_mhz));
      prv_modify(rcc, STM32U5_RCC_CFGR2_OFFSET, RCC_CFGR2_HPRE_MASK, (uint32_t)pre);
      prv_hclk_changed(ctrl);
      return 0;
    case C(PCLK1):
    case C(PCLK2):
    case C(PCLK3):
      pre = prv_apb_pre(div);
      if (pre < 0) {
        return -EINVAL;
      }
      if (id == C(PCLK3)) {
        prv_modify(rcc, STM32U5_RCC_CFGR3_OFFSET, RCC_CFGR3_PPRE3_MASK, (uint32_t)pre);
      } else {
        prv_modify(rcc, STM32U5_RCC_CFGR2_OFFSET,
                   id == C(PCLK1) ? RCC_CFGR2_PPRE1_MASK : RCC_CFGR2_PPRE2_MASK, (uint32_t)pre);
      }
      return 0;
    default:
      return -EINVAL;
  }
}

uint32_t pbl_clk_stm32u5_reset_flags(const struct pbl_clk_controller *ctrl, bool clear) {
  const struct pbl_clk_stm32u5 *rcc = prv_rcc(ctrl);
  uint32_t csr = prv_getreg(rcc, STM32U5_RCC_CSR_OFFSET);

  if (clear) {
    pbl_modifyl(0U, RCC_CSR_RMVF, rcc->base + STM32U5_RCC_CSR_OFFSET);
  }

  return csr;
}

const struct pbl_clk_controller_ops pbl_clk_stm32u5_ops = {
  .enable = prv_enable,
  .disable = prv_disable,
  .get_rate = prv_get_rate,
  .get_parent = prv_get_parent,
  .set_parent = prv_set_parent,
  .set_rate = prv_set_rate,
};

// Range 1 with the EPOD booster, before any clock gets faster than 25 MHz
static int prv_vos1(const struct pbl_clk_stm32u5 *rcc) {
  uintptr_t vosr = rcc->pwr + STM32U5_PWR_VOSR_OFFSET;
  uintptr_t svmsr = rcc->pwr + STM32U5_PWR_SVMSR_OFFSET;
  int ret;

  if (PBL_FIELD_GET(PWR_SVMSR_ACTVOS_MASK, pbl_readl(svmsr)) == PWR_VOSR_VOS_1 &&
      PBL_FIELD_GET(PWR_VOSR_VOS_MASK, pbl_readl(vosr)) == PWR_VOSR_VOS_1) {
    pbl_modifyl(0U, PWR_VOSR_BOOSTEN, vosr);
    return 0;
  }

  pbl_modifyl(PWR_VOSR_VOS_MASK,
              PBL_FIELD_PREP(PWR_VOSR_VOS_MASK, PWR_VOSR_VOS_1) | PWR_VOSR_BOOSTEN, vosr);
  ret = PBL_POLL_BUSY_US((pbl_readl(vosr) & PWR_VOSR_VOSRDY) != 0U, 1U, VOS_TIMEOUT_US);
  if (ret != 0) {
    return ret;
  }

  return PBL_POLL_BUSY_US((pbl_readl(svmsr) & PWR_SVMSR_ACTVOSRDY) != 0U, 1U, VOS_TIMEOUT_US);
}

int pbl_clk_stm32u5_init(const struct pbl_device *dev) {
  const struct pbl_clk_stm32u5 *rcc = PBL_CONTAINER_OF(dev, const struct pbl_clk_stm32u5, ctrl.dev);
  uintptr_t cr3 = rcc->pwr + STM32U5_PWR_CR3_OFFSET;
  uintptr_t svmsr = rcc->pwr + STM32U5_PWR_SVMSR_OFFSET;
  int ret;

  pbl_modifyl(0U, RCC_AHB3ENR_PWREN, rcc->base + STM32U5_RCC_AHB3ENR_OFFSET);
  (void)prv_getreg(rcc, STM32U5_RCC_AHB3ENR_OFFSET);

  // The supply the board is wired for
  if (rcc->supply == PBL_CLK_STM32U5_SUPPLY_SMPS) {
    pbl_modifyl(0U, PWR_CR3_REGSEL, cr3);
    ret = PBL_POLL_BUSY_US((pbl_readl(svmsr) & PWR_SVMSR_REGS) != 0U, 1U, VOS_TIMEOUT_US);
  } else {
    pbl_modifyl(PWR_CR3_REGSEL, 0U, cr3);
    ret = PBL_POLL_BUSY_US((pbl_readl(svmsr) & PWR_SVMSR_REGS) == 0U, 1U, VOS_TIMEOUT_US);
  }
  if (ret != 0) {
    return ret;
  }

  ret = prv_vos1(rcc);
  if (ret != 0) {
    return ret;
  }

  // MSIS and MSIK take the ranges set here, rather than those for after Standby
  if ((prv_getreg(rcc, STM32U5_RCC_ICSCR1_OFFSET) & RCC_ICSCR1_MSIRGSEL) == 0U) {
    uint32_t csr = prv_getreg(rcc, STM32U5_RCC_CSR_OFFSET);

    pbl_modifyl(
        RCC_ICSCR1_MSISRANGE_MASK | RCC_ICSCR1_MSIKRANGE_MASK,
        PBL_FIELD_PREP(RCC_ICSCR1_MSISRANGE_MASK, PBL_FIELD_GET(RCC_CSR_MSISSRANGE_MASK, csr)) |
            PBL_FIELD_PREP(RCC_ICSCR1_MSIKRANGE_MASK, PBL_FIELD_GET(RCC_CSR_MSIKSRANGE_MASK, csr)) |
            RCC_ICSCR1_MSIRGSEL,
        rcc->base + STM32U5_RCC_ICSCR1_OFFSET);
  }

  // The early init left the system clock on MSIS: PLL1 starts again from what it is assigned
  ret = prv_pll1_off(rcc);
  if (ret != 0) {
    return ret;
  }

  prv_update_core_clock(&rcc->ctrl);

  return pbl_clk_assign(rcc->assigned);
}

#include PBL_DT_INSTANCES(st_stm32u5_rcc_clk)
