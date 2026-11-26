/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Polled I2C masters on the four buses of the board, from HSI16. 100 kHz by
 * default, 400 kHz on request; the timings are RM0456's for 16 MHz.
 */

#include "regs.h"
#include "stub.h"

#define TIMING_100K 0x30420f13U
#define TIMING_400K 0x10320309U

/* The STUSB4500 may hold I2C1 low for 250 ms after an unplug */
#define BUSY_TIMEOUT_MS 300U
#define BYTE_TIMEOUT_MS 25U

struct bus {
  uintptr_t base;
  uintptr_t enr, rstr;
  uint32_t bit;
  uintptr_t ccipr;
  uint32_t sel_mask;
  uint8_t port, scl, sda;
};

static const struct bus s_buses[4] = {
  {I2C1_BASE, RCC(STM32U5_RCC_APB1ENR1_OFFSET), RCC(RCC_APB1RSTR1_OFFSET), RCC_APB1ENR1_I2C1,
   RCC(STM32U5_RCC_CCIPR1_OFFSET), RCC_CCIPR1_I2C1SEL_MASK, PORT_B, 6, 7},
  {I2C2_BASE, RCC(STM32U5_RCC_APB1ENR1_OFFSET), RCC(RCC_APB1RSTR1_OFFSET), RCC_APB1ENR1_I2C2,
   RCC(STM32U5_RCC_CCIPR1_OFFSET), RCC_CCIPR1_I2C2SEL_MASK, PORT_B, 13, 14},
  {I2C3_BASE, RCC(STM32U5_RCC_APB3ENR_OFFSET), RCC(RCC_APB3RSTR_OFFSET), RCC_APB3_I2C3,
   RCC(STM32U5_RCC_CCIPR3_OFFSET), RCC_CCIPR3_I2C3SEL_MASK, PORT_C, 0, 1},
  {I2C4_BASE, RCC(STM32U5_RCC_APB1ENR2_OFFSET), RCC(RCC_APB1RSTR2_OFFSET), RCC_APB1ENR2_I2C4,
   RCC(STM32U5_RCC_CCIPR1_OFFSET), RCC_CCIPR1_I2C4SEL_MASK, PORT_D, 12, 13},
};

static uint8_t s_ready; /* bit per bus */
static uint8_t s_fast;

#define I2C(b, off) ((b)->base + (off))

/* A device left mid-byte holds SDA low: clock it out, then a stop */
static void prv_recover(const struct bus *b) {
  gpio_write(b->port, b->scl, true);
  gpio_write(b->port, b->sda, true);
  gpio_config(b->port, b->scl, GPIO_OUT, true, PULL_NONE, 0U);
  gpio_config(b->port, b->sda, GPIO_OUT, true, PULL_NONE, 0U);
  delay_us(10U);
  for (unsigned i = 0U; i < 9U && !gpio_read(b->port, b->sda); i++) {
    gpio_write(b->port, b->scl, false);
    delay_us(5U);
    gpio_write(b->port, b->scl, true);
    delay_us(5U);
  }
  gpio_write(b->port, b->sda, false);
  delay_us(5U);
  gpio_write(b->port, b->sda, true);
  delay_us(5U);
}

static void prv_init(int n, bool fast) {
  const struct bus *b = &s_buses[n];

  setb(b->enr, b->bit);
  (void)rd(b->enr);
  setb(b->rstr, b->bit);
  (void)rd(b->rstr);
  clrb(b->rstr, b->bit);
  modb(b->ccipr, b->sel_mask, PBL_FIELD_PREP(b->sel_mask, RCC_I2CSEL_HSI16));

  prv_recover(b);
  gpio_config(b->port, b->scl, GPIO_AF, true, PULL_NONE, 4U);
  gpio_config(b->port, b->sda, GPIO_AF, true, PULL_NONE, 4U);

  wr(I2C(b, STM32_I2C_CR1_OFFSET), 0U);
  wr(I2C(b, STM32_I2C_TIMINGR_OFFSET), fast ? TIMING_400K : TIMING_100K);
  wr(I2C(b, STM32_I2C_CR1_OFFSET), I2C_CR1_PE);
  s_ready |= (uint8_t)(1U << n);
  s_fast = (uint8_t)((s_fast & ~(1U << n)) | (fast ? (1U << n) : 0U));
}

static int prv_wait(const struct bus *b, uint32_t flag) {
  uint32_t start = millis();

  for (;;) {
    uint32_t isr = rd(I2C(b, STM32_I2C_ISR_OFFSET));
    /* A NACK comes with the stop it causes: it goes first */
    if (isr & I2C_ISR_NACKF) {
      return TETHER_ERR_NACK;
    }
    if (isr & I2C_ISR_ERRORS) {
      return TETHER_ERR_BUS;
    }
    if (isr & flag) {
      return TETHER_OK;
    }
    if (millis() - start > BYTE_TIMEOUT_MS) {
      return TETHER_ERR_TIMEOUT;
    }
  }
}

/* The end of a transfer, failed or not: a stop, then the flags cleared */
static int prv_finish(const struct bus *b, int ret) {
  uint32_t start = millis();

  if (ret != TETHER_OK && (rd(I2C(b, STM32_I2C_ISR_OFFSET)) & I2C_ISR_STOPF) == 0U) {
    setb(I2C(b, STM32_I2C_CR2_OFFSET), I2C_CR2_STOP);
  }
  while ((rd(I2C(b, STM32_I2C_ISR_OFFSET)) & I2C_ISR_STOPF) == 0U) {
    if (millis() - start > BYTE_TIMEOUT_MS) {
      /* Wedged: off and on again */
      clrb(I2C(b, STM32_I2C_CR1_OFFSET), I2C_CR1_PE);
      (void)rd(I2C(b, STM32_I2C_CR1_OFFSET));
      setb(I2C(b, STM32_I2C_CR1_OFFSET), I2C_CR1_PE);
      if (ret == TETHER_OK) {
        ret = TETHER_ERR_TIMEOUT;
      }
      break;
    }
  }
  wr(I2C(b, STM32_I2C_ICR_OFFSET), I2C_ISR_STOPF | I2C_ISR_NACKF | I2C_ISR_ERRORS);
  return ret;
}

/* Writes wr, then reads rd after a repeated start; neither: an address probe */
int i2c_xfer(int bus, uint8_t addr, const uint8_t *wrb, uint32_t wn, uint8_t *rdb, uint32_t rn,
             bool fast) {
  if (bus < 1 || bus > 4 || addr > 0x7fU || wn > TETHER_I2C_MAX || rn > TETHER_I2C_MAX) {
    return TETHER_ERR_ARG;
  }
  const int n = bus - 1;
  const struct bus *b = &s_buses[n];
  const uint32_t sadd = (uint32_t)addr << 1;
  int ret = TETHER_OK;

  if ((s_ready & (1U << n)) == 0U || ((s_fast >> n) & 1U) != (fast ? 1U : 0U)) {
    prv_init(n, fast);
  }

  uint32_t start = millis();
  while (rd(I2C(b, STM32_I2C_ISR_OFFSET)) & I2C_ISR_BUSY) {
    if (millis() - start > BUSY_TIMEOUT_MS) {
      prv_init(n, fast);
      if (rd(I2C(b, STM32_I2C_ISR_OFFSET)) & I2C_ISR_BUSY) {
        return TETHER_ERR_BUS;
      }
      break;
    }
  }
  wr(I2C(b, STM32_I2C_ICR_OFFSET), I2C_ISR_STOPF | I2C_ISR_NACKF | I2C_ISR_ERRORS);

  if (wn != 0U || rn == 0U) {
    wr(I2C(b, STM32_I2C_CR2_OFFSET), sadd | PBL_FIELD_PREP(I2C_CR2_NBYTES_MASK, wn) |
                                         (rn == 0U ? (1U << 25) : 0U) | I2C_CR2_START);
    for (uint32_t i = 0U; i < wn && ret == TETHER_OK; i++) {
      ret = prv_wait(b, I2C_ISR_TXIS);
      if (ret == TETHER_OK) {
        wr(I2C(b, STM32_I2C_TXDR_OFFSET), wrb[i]);
      }
    }
    if (ret == TETHER_OK && rn != 0U) {
      ret = prv_wait(b, I2C_ISR_TC);
    }
    if (ret == TETHER_OK && rn == 0U) {
      ret = prv_wait(b, I2C_ISR_STOPF);
    }
  }
  if (ret == TETHER_OK && rn != 0U) {
    wr(I2C(b, STM32_I2C_CR2_OFFSET), sadd | I2C_CR2_RD_WRN |
                                         PBL_FIELD_PREP(I2C_CR2_NBYTES_MASK, rn) | (1U << 25) |
                                         I2C_CR2_START);
    for (uint32_t i = 0U; i < rn && ret == TETHER_OK; i++) {
      ret = prv_wait(b, I2C_ISR_RXNE);
      if (ret == TETHER_OK) {
        rdb[i] = (uint8_t)rd(I2C(b, STM32_I2C_RXDR_OFFSET));
      }
    }
    if (ret == TETHER_OK) {
      ret = prv_wait(b, I2C_ISR_STOPF);
    }
  }
  return prv_finish(b, ret);
}
