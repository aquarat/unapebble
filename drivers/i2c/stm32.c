/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/i2c/stm32.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/util/bits.h>
#include <pbl/util/math.h>
#include <pbl/util/misc.h>

#include <hardware/stm32_i2c.h>

#define CR1_IRQS \
  (I2C_CR1_TXIE | I2C_CR1_RXIE | I2C_CR1_NACKIE | I2C_CR1_STOPIE | I2C_CR1_TCIE | I2C_CR1_ERRIE)

// Clock rates are in mHz
#define MHZ_PER_HZ 1000ULL

#define NS_PER_S 1000000000ULL

// Minimum SCL low and high times and data setup time of each mode, in ns
struct timing {
  uint32_t max_hz;
  uint32_t low_ns;
  uint32_t high_ns;
  uint32_t setup_ns;
};

static const struct timing s_timings[] = {
  {.max_hz = 100000U, .low_ns = 4700U, .high_ns = 4000U, .setup_ns = 250U},
  {.max_hz = 400000U, .low_ns = 1300U, .high_ns = 600U, .setup_ns = 100U},
  {.max_hz = 1000000U, .low_ns = 500U, .high_ns = 260U, .setup_ns = 50U},
};

static const struct pbl_i2c_stm32 *prv_i2c(const struct pbl_i2c_bus *bus) {
  return PBL_CONTAINER_OF(bus, const struct pbl_i2c_stm32, bus);
}

static inline uint32_t prv_getreg(const struct pbl_i2c_stm32 *i2c, uint32_t offset) {
  return pbl_readl(i2c->base + offset);
}

static inline void prv_putreg(const struct pbl_i2c_stm32 *i2c, uint32_t offset, uint32_t value) {
  pbl_writel(value, i2c->base + offset);
}

static uint32_t prv_ticks(uint64_t hz, uint32_t ns) {
  return (uint32_t)((hz * ns + NS_PER_S - 1U) / NS_PER_S);
}

// The smallest prescaler whose SCL low and high periods fit, splitting what the bus period leaves
// over the minimums between them
static int prv_timingr(uint64_t clk_hz, uint32_t bus_hz, uint32_t *timingr) {
  const struct timing *t = nullptr;

  for (size_t i = 0U; i < PBL_ARRAY_SIZE(s_timings); i++) {
    if (bus_hz <= s_timings[i].max_hz) {
      t = &s_timings[i];
      break;
    }
  }
  if (t == nullptr || bus_hz == 0U) {
    return -EINVAL;
  }

  for (uint32_t presc = 0U; presc < 16U; presc++) {
    uint64_t hz = clk_hz / (presc + 1U);
    uint32_t low = prv_ticks(hz, t->low_ns);
    uint32_t high = prv_ticks(hz, t->high_ns);
    uint32_t total = (uint32_t)((hz + bus_hz - 1U) / bus_hz);
    uint32_t setup = PBL_MAX(prv_ticks(hz, t->setup_ns), 1U);
    uint32_t extra = total > low + high ? total - low - high : 0U;

    low += extra / 2U;
    high += extra - extra / 2U;
    if (low > 256U || high > 256U || setup > 16U) {
      continue;
    }

    *timingr = PBL_FIELD_PREP(I2C_TIMINGR_PRESC_MASK, presc) |
               PBL_FIELD_PREP(I2C_TIMINGR_SCLDEL_MASK, setup - 1U) |
               PBL_FIELD_PREP(I2C_TIMINGR_SCLH_MASK, high - 1U) |
               PBL_FIELD_PREP(I2C_TIMINGR_SCLL_MASK, low - 1U);
    return 0;
  }

  return -EINVAL;
}

static int prv_init(const struct pbl_i2c_bus *bus) {
  const struct pbl_i2c_stm32 *i2c = prv_i2c(bus);
  uint64_t rate_mhz;
  int ret;

  ret = pbl_device_init(&i2c->clk.ctrl->dev);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_clk_get_rate(&i2c->clk, &rate_mhz);
  if (ret != 0) {
    return ret;
  }

  ret = prv_timingr(rate_mhz / MHZ_PER_HZ, i2c->clock_hz, &i2c->state->timingr);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_pinctrl_select_state(&i2c->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
  if (ret != 0) {
    return ret;
  }

  pbl_irq_enable(i2c->irq_ev);
  pbl_irq_enable(i2c->irq_er);

  return 0;
}

// TIMINGR only takes writes with the controller disabled
static void prv_enable(const struct pbl_i2c_bus *bus) {
  const struct pbl_i2c_stm32 *i2c = prv_i2c(bus);

  (void)pbl_clk_enable(&i2c->clk);
  prv_putreg(i2c, STM32_I2C_CR1_OFFSET, 0U);
  prv_putreg(i2c, STM32_I2C_TIMINGR_OFFSET, i2c->state->timingr);
  prv_putreg(i2c, STM32_I2C_CR1_OFFSET, I2C_CR1_PE);
}

// Disabling resets the state machine and the flags; PE stays low for three APB cycles
static void prv_reset(const struct pbl_i2c_stm32 *i2c) {
  prv_putreg(i2c, STM32_I2C_CR1_OFFSET, 0U);
  for (uint32_t i = 0U; i < 3U; i++) {
    (void)prv_getreg(i2c, STM32_I2C_CR1_OFFSET);
  }
  pbl_irq_clear_pending(i2c->irq_ev);
  pbl_irq_clear_pending(i2c->irq_er);
}

static void prv_disable(const struct pbl_i2c_bus *bus) {
  const struct pbl_i2c_stm32 *i2c = prv_i2c(bus);

  prv_reset(i2c);
  (void)pbl_clk_disable(&i2c->clk);
}

static void prv_abort(const struct pbl_i2c_bus *bus) {
  const struct pbl_i2c_stm32 *i2c = prv_i2c(bus);

  prv_reset(i2c);
  prv_putreg(i2c, STM32_I2C_CR1_OFFSET, I2C_CR1_PE);
}

static bool prv_reading(const struct pbl_i2c_stm32_state *state) {
  return (state->msgs[state->idx].flags & PBL_I2C_MSG_READ) != 0U;
}

// The bytes of a START: its message, and the writes that continue it
static uint32_t prv_seg_len(const struct pbl_i2c_stm32_state *state) {
  uint32_t len = state->msgs[state->idx].len;

  if (!prv_reading(state)) {
    for (uint8_t i = state->idx + 1U;
         i < state->num_msgs && (state->msgs[i].flags & PBL_I2C_MSG_NOSTART) != 0U; i++) {
      len += state->msgs[i].len;
    }
  }

  return len;
}

static uint32_t prv_chunk(const struct pbl_i2c_stm32_state *state) {
  uint32_t chunk = PBL_MIN(state->seg_left, I2C_CR2_NBYTES_MAX);

  return PBL_FIELD_PREP(I2C_CR2_NBYTES_MASK, chunk) |
         (state->seg_left > chunk ? I2C_CR2_RELOAD : 0U);
}

// A START, or a repeated START, with the address and the first chunk
static void prv_seg_start(const struct pbl_i2c_stm32 *i2c) {
  struct pbl_i2c_stm32_state *state = i2c->state;

  state->pos = 0U;
  state->seg_left = prv_seg_len(state);
  state->chunk_left = PBL_MIN(state->seg_left, I2C_CR2_NBYTES_MAX);
  state->seg_moved = false;

  prv_putreg(i2c, STM32_I2C_CR2_OFFSET,
             PBL_FIELD_PREP(I2C_CR2_SADD_MASK, (uint32_t)state->addr << 1) |
                 (prv_reading(state) ? I2C_CR2_RD_WRN : 0U) | prv_chunk(state) | I2C_CR2_START);
}

// The message the next byte of the START is in, past those it emptied
static struct pbl_i2c_msg *prv_cur(struct pbl_i2c_stm32_state *state) {
  while (state->pos == state->msgs[state->idx].len && state->idx + 1U < state->num_msgs &&
         (state->msgs[state->idx + 1U].flags & PBL_I2C_MSG_NOSTART) != 0U) {
    state->idx++;
    state->pos = 0U;
  }

  return &state->msgs[state->idx];
}

static int prv_start(const struct pbl_i2c_bus *bus, uint16_t addr, struct pbl_i2c_msg *msgs,
                     uint8_t num_msgs) {
  const struct pbl_i2c_stm32 *i2c = prv_i2c(bus);
  struct pbl_i2c_stm32_state *state = i2c->state;

  if ((prv_getreg(i2c, STM32_I2C_ISR_OFFSET) & I2C_ISR_BUSY) != 0U) {
    return -EBUSY;
  }

  state->msgs = msgs;
  state->num_msgs = num_msgs;
  state->idx = 0U;
  state->addr = addr;
  state->error = 0;

  prv_putreg(i2c, STM32_I2C_ICR_OFFSET, I2C_ISR_NACKF | I2C_ISR_STOPF | I2C_ISR_ERRORS);
  pbl_modifyl(0U, CR1_IRQS, i2c->base + STM32_I2C_CR1_OFFSET);
  prv_seg_start(i2c);

  return 0;
}

static void prv_done(const struct pbl_i2c_stm32 *i2c, int status) {
  pbl_modifyl(CR1_IRQS, 0U, i2c->base + STM32_I2C_CR1_OFFSET);
  pbl_i2c_bus_complete(&i2c->bus, status);
}

// The next chunk of a START, or what follows it: a repeated START or the STOP
static void prv_chunk_done(const struct pbl_i2c_stm32 *i2c, uint32_t isr) {
  struct pbl_i2c_stm32_state *state = i2c->state;

  if ((isr & I2C_ISR_TCR) != 0U) {
    state->chunk_left = PBL_MIN(state->seg_left, I2C_CR2_NBYTES_MAX);
    pbl_modifyl(I2C_CR2_NBYTES_MASK | I2C_CR2_RELOAD, prv_chunk(state),
                i2c->base + STM32_I2C_CR2_OFFSET);
    return;
  }

  // The last message of the START is done: the writes it continued were moved on to
  (void)prv_cur(state);
  if (state->idx + 1U < state->num_msgs) {
    state->idx++;
    prv_seg_start(i2c);
  } else {
    pbl_modifyl(0U, I2C_CR2_STOP, i2c->base + STM32_I2C_CR2_OFFSET);
  }
}

void pbl_i2c_stm32_irq_handler(const struct pbl_i2c_stm32 *i2c) {
  struct pbl_i2c_stm32_state *state = i2c->state;
  uint32_t isr = prv_getreg(i2c, STM32_I2C_ISR_OFFSET);

  if ((prv_getreg(i2c, STM32_I2C_CR1_OFFSET) & CR1_IRQS) == 0U) {
    return;
  }

  // The controller lost the bus, or saw a misplaced START or STOP: no STOP follows
  if ((isr & I2C_ISR_ERRORS) != 0U) {
    prv_putreg(i2c, STM32_I2C_ICR_OFFSET, isr & I2C_ISR_ERRORS);
    prv_abort(&i2c->bus);
    prv_done(i2c, -EIO);
    return;
  }

  // The controller sends the STOP itself on a NACK
  if ((isr & I2C_ISR_NACKF) != 0U) {
    prv_putreg(i2c, STM32_I2C_ICR_OFFSET, I2C_ISR_NACKF);
    prv_putreg(i2c, STM32_I2C_ISR_OFFSET, I2C_ISR_TXE);
    if (state->error == 0) {
      state->error = state->seg_moved ? -EIO : -ENXIO;
    }
  }

  if ((isr & I2C_ISR_STOPF) != 0U) {
    prv_putreg(i2c, STM32_I2C_ICR_OFFSET, I2C_ISR_STOPF);
    if (state->error == 0 && (state->seg_left != 0U || state->idx + 1U < state->num_msgs)) {
      state->error = -EIO;
    }
    prv_done(i2c, state->error);
    return;
  }

  if (state->error != 0) {
    return;
  }

  if ((isr & I2C_ISR_RXNE) != 0U && state->chunk_left > 0U) {
    struct pbl_i2c_msg *msg = &state->msgs[state->idx];

    msg->buf[state->pos++] = (uint8_t)prv_getreg(i2c, STM32_I2C_RXDR_OFFSET);
    state->seg_left--;
    state->chunk_left--;
    state->seg_moved = true;
  }

  if ((isr & I2C_ISR_TXIS) != 0U && state->chunk_left > 0U) {
    struct pbl_i2c_msg *msg = prv_cur(state);

    prv_putreg(i2c, STM32_I2C_TXDR_OFFSET, msg->buf[state->pos++]);
    state->seg_left--;
    state->chunk_left--;
    state->seg_moved = true;
  }

  if ((isr & (I2C_ISR_TC | I2C_ISR_TCR)) != 0U) {
    prv_chunk_done(i2c, isr);
  }
}

const struct pbl_i2c_bus_ops pbl_i2c_stm32_ops = {
  .init = prv_init,
  .enable = prv_enable,
  .disable = prv_disable,
  .start = prv_start,
  .abort = prv_abort,
};

#include PBL_DT_INSTANCES(st_stm32f7_i2c)
