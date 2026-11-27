/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * STM32U5A5 registers the stub uses, non-secure aliases (TrustZone is off). The
 * fields come from soc/st/include/hardware where PebbleOS has them; the rest
 * from ST's stm32u5a5xx.h and RM0456.
 */

#pragma once

#include <stdint.h>

#include <hardware/stm32_gpio.h>
#include <hardware/stm32_i2c.h>
#include <hardware/stm32_iwdg.h>
#include <hardware/stm32_lptim.h>
#include <hardware/stm32u5_flash.h>
#include <hardware/stm32u5_icache.h>
#include <hardware/stm32u5_pwr.h>
#include <hardware/stm32u5_rcc.h>

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

static inline uint32_t rd(uintptr_t a) {
  return REG(a);
}
static inline void wr(uintptr_t a, uint32_t v) {
  REG(a) = v;
}
static inline void setb(uintptr_t a, uint32_t m) {
  REG(a) |= m;
}
static inline void clrb(uintptr_t a, uint32_t m) {
  REG(a) &= ~m;
}
static inline void modb(uintptr_t a, uint32_t clear, uint32_t set) {
  REG(a) = (REG(a) & ~clear) | set;
}

#define RCC_BASE        0x46020c00U
#define PWR_BASE        0x46020800U
#define FLASH_BASE      0x40022000U
#define EXTI_BASE       0x46022000U
#define ICACHE_BASE     0x40030400U
#define SYSCFG_BASE     0x46000400U
#define OTG_BASE        0x42040000U
#define IWDG_BASE       0x40003000U
#define TAMP_BASE       0x46007c00U
#define LPTIM1_BASE     0x46004400U
#define LPTIM3_BASE     0x46004800U
#define I2C1_BASE       0x40005400U
#define I2C2_BASE       0x40005800U
#define I2C3_BASE       0x46002800U
#define I2C4_BASE       0x40008400U
#define GPIO_BASE(port) (0x42020000U + ((uint32_t)(port) * 0x400U))

#define UID_BASE      0x0bfa0700U
#define DBGMCU_IDCODE 0xe0044000U

#define RCC(off)    (RCC_BASE + (off))
#define PWR(off)    (PWR_BASE + (off))
#define FLASHR(off) (FLASH_BASE + (off))

/* RCC registers and bits PebbleOS's header leaves out */
#define RCC_AHB2RSTR1_OFFSET 0x0064U
#define RCC_AHB3RSTR_OFFSET  0x006cU
#define RCC_APB1RSTR1_OFFSET 0x0074U
#define RCC_APB1RSTR2_OFFSET 0x0078U
#define RCC_APB3RSTR_OFFSET  0x0080U
#define RCC_CFGR1_SWS_SHIFT  2U

#define RCC_AHB1ENR_FLASHEN        (1U << 8)
#define RCC_AHB1ENR_CRCEN          (1U << 12)
#define RCC_AHB2_OTG               (1U << 14)
#define RCC_AHB2_USBPHYC           (1U << 15)
#define RCC_AHB2_SDMMC1            (1U << 27)
#define RCC_APB3_SYSCFG            (1U << 1)
#define RCC_APB3_I2C3              (1U << 7)
#define RCC_APB3_LPTIM1            (1U << 11)
#define RCC_APB3_LPTIM3            (1U << 12)
#define RCC_APB1ENR1_I2C1          (1U << 21)
#define RCC_APB1ENR1_I2C2          (1U << 22)
#define RCC_APB1ENR2_I2C4          (1U << 1)

#define PWR_VOSR_USBPWREN   (1U << 19)
#define PWR_VOSR_USBBOOSTEN (1U << 20)
#define PWR_SVMCR_USV       (1U << 28)

#define SYSCFG_OTGHSPHYCR              (SYSCFG_BASE + 0x74U)
#define SYSCFG_OTGHSPHYCR_EN           (1U << 0)
#define SYSCFG_OTGHSPHYCR_CLKSEL_16MHZ (3U << 2)
#define SYSCFG_OTGHSPHYCR_CLKSEL_MASK  (0xfU << 2)

#define TAMP_BKP3R (TAMP_BASE + 0x100U + 3U * 4U)

/* System control space */
#define SCB_ICSR     0xe000ed04U
#define SCB_VTOR     0xe000ed08U
#define SCB_AIRCR    0xe000ed0cU
#define SCB_CCR      0xe000ed14U
#define SCB_SHPR3    0xe000ed20U
#define SCB_SHCSR    0xe000ed24U
#define SCB_CFSR     0xe000ed28U
#define SCB_HFSR     0xe000ed2cU
#define SCB_MMFAR    0xe000ed34U
#define SCB_BFAR     0xe000ed38U
#define SCB_AFSR     0xe000ed3cU
#define SCB_CPACR    0xe000ed88U
#define SCB_DEMCR    0xe000edfcU
#define FPU_FPCCR    0xe000ef34U
#define NVIC_ICER(n) (0xe000e180U + 4U * (n))
#define NVIC_ICPR(n) (0xe000e280U + 4U * (n))
#define SYST_CSR     0xe000e010U
#define SYST_RVR     0xe000e014U
#define SYST_CVR     0xe000e018U
#define DWT_CTRL     0xe0001000U
#define DWT_CYCCNT   0xe0001004U

#define AIRCR_VECTKEY     (0x05faU << 16)
#define AIRCR_SYSRESETREQ (1U << 2)

#define NUM_IRQS 141U

/* GPIO ports */
enum {
  PORT_A,
  PORT_B,
  PORT_C,
  PORT_D,
  PORT_E,
  PORT_F,
  PORT_G,
  PORT_H,
  PORT_I,
  NUM_PORTS
};

/* USB OTG HS (DWC2) */
#define OTG(off)        (OTG_BASE + (off))
#define OTG_GOTGCTL     OTG(0x000U)
#define OTG_GAHBCFG     OTG(0x008U)
#define OTG_GUSBCFG     OTG(0x00cU)
#define OTG_GRSTCTL     OTG(0x010U)
#define OTG_GINTSTS     OTG(0x014U)
#define OTG_GINTMSK     OTG(0x018U)
#define OTG_GRXSTSP     OTG(0x020U)
#define OTG_GRXFSIZ     OTG(0x024U)
#define OTG_DIEPTXF0    OTG(0x028U)
#define OTG_GCCFG       OTG(0x038U)
#define OTG_GSNPSID     OTG(0x040U)
#define OTG_DIEPTXF(n)  OTG(0x104U + 4U * ((n) - 1U))
#define OTG_DCFG        OTG(0x800U)
#define OTG_DCTL        OTG(0x804U)
#define OTG_DSTS        OTG(0x808U)
#define OTG_DIEPMSK     OTG(0x810U)
#define OTG_DOEPMSK     OTG(0x814U)
#define OTG_DAINTMSK    OTG(0x81cU)
#define OTG_DIEPCTL(n)  OTG(0x900U + 0x20U * (n))
#define OTG_DIEPINT(n)  OTG(0x908U + 0x20U * (n))
#define OTG_DIEPTSIZ(n) OTG(0x910U + 0x20U * (n))
#define OTG_DTXFSTS(n)  OTG(0x918U + 0x20U * (n))
#define OTG_DOEPCTL(n)  OTG(0xb00U + 0x20U * (n))
#define OTG_DOEPINT(n)  OTG(0xb08U + 0x20U * (n))
#define OTG_DOEPTSIZ(n) OTG(0xb10U + 0x20U * (n))
#define OTG_PCGCCTL     OTG(0xe00U)
#define OTG_FIFO(n)     OTG(0x1000U + 0x1000U * (n))

#define GAHBCFG_GINT         (1U << 0)
#define GUSBCFG_TRDT_SHIFT   10U
#define GUSBCFG_TRDT_MASK    (0xfU << 10)
#define GUSBCFG_TSDPS        (1U << 22)
#define GUSBCFG_FHMOD        (1U << 29)
#define GUSBCFG_FDMOD        (1U << 30)
#define GRSTCTL_CSRST        (1U << 0)
#define GRSTCTL_RXFFLSH      (1U << 4)
#define GRSTCTL_TXFFLSH      (1U << 5)
#define GRSTCTL_TXFNUM_SHIFT 6U
#define GRSTCTL_AHBIDL       (1U << 31)
#define GINTSTS_CMOD         (1U << 0)
#define GINTSTS_RXFLVL       (1U << 4)
#define GINTSTS_USBSUSP      (1U << 11)
#define GINTSTS_USBRST       (1U << 12)
#define GINTSTS_ENUMDNE      (1U << 13)
#define GINTSTS_IEPINT       (1U << 18)
#define GINTSTS_OEPINT       (1U << 19)
#define GINTSTS_WKUPINT      (1U << 31)
#define GRXSTSP_EPNUM(v)     ((v) & 0xfU)
#define GRXSTSP_BCNT(v)      (((v) >> 4) & 0x7ffU)
#define GRXSTSP_PKTSTS(v)    (((v) >> 17) & 0xfU)
#define PKTSTS_OUT_DATA      2U
#define PKTSTS_OUT_DONE      3U
#define PKTSTS_SETUP_DONE    4U
#define PKTSTS_SETUP_DATA    6U
#define GCCFG_VBDEN          (1U << 21)
#define GCCFG_VBVALOVAL      (1U << 23)
#define GCCFG_VBVALEXTOEN    (1U << 24)
#define GCCFG_PULLDOWNEN     (1U << 25)
#define DCFG_DSPD_MASK       3U
#define DCFG_DSPD_HS         0U
#define DCFG_DSPD_HS_IN_FS   1U
#define DCFG_DAD_SHIFT       4U
#define DCFG_DAD_MASK        (0x7fU << 4)
#define DCTL_SDIS            (1U << 1)
#define DCTL_CGINAK          (1U << 8)
#define DCTL_CGONAK          (1U << 10)
#define DSTS_ENUMSPD(v)      (((v) >> 1) & 3U)
#define EPCTL_USBAEP         (1U << 15)
#define EPCTL_EPTYP_SHIFT    18U
#define EPCTL_STALL          (1U << 21)
#define EPCTL_TXFNUM_SHIFT   22U
#define EPCTL_CNAK           (1U << 26)
#define EPCTL_SNAK           (1U << 27)
#define EPCTL_SD0PID         (1U << 28)
#define EPCTL_EPDIS          (1U << 30)
#define EPCTL_EPENA          (1U << 31)
#define EPINT_XFRC           (1U << 0)
#define EPINT_EPDISD         (1U << 1)
#define DOEPINT_STUP         (1U << 3)
#define DOEPINT_STPKTRX      (1U << 15)
#define EPTSIZ_PKTCNT_SHIFT  19U
#define DOEPTSIZ0_STUPCNT3   (3U << 29)
