/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/spi/stm32.h>
#include <pbl/kernel/idle.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/soc/st/idle.h>
#include <pbl/util/bits.h>
#include <pbl/util/misc.h>

#include <hardware/stm32_spi.h>

// Clock rates are in mHz
#define MHZ_PER_HZ 1000ULL

// Bytes written ahead of those read: within the smallest FIFO, of 8 bytes, so the receive side
// never overruns
#define TX_AHEAD 8U

// The last bit is in well before this many polls of SR once the last byte is read
#define EOT_POLLS 1000U

#define IER_IRQS (SPI_IER_RXPIE | SPI_IER_TXPIE | SPI_IER_EOTIE | SPI_IER_OVRIE | SPI_IER_MODFIE)

static const struct pbl_spi_stm32 *prv_spi(const struct pbl_spi_bus *bus) {
  return PBL_CONTAINER_OF(bus, const struct pbl_spi_stm32, bus);
}

static inline uint32_t prv_getreg(const struct pbl_spi_stm32 *spi, uint32_t offset) {
  return pbl_readl(spi->base + offset);
}

static inline void prv_putreg(const struct pbl_spi_stm32 *spi, uint32_t offset, uint32_t value) {
  pbl_writel(value, spi->base + offset);
}

static void prv_idle_pins(const struct pbl_spi_stm32 *spi) {
  if (spi->pinctrl.states[PBL_PINCTRL_STATE_SLEEP] != nullptr) {
    (void)pbl_pinctrl_select_state(&spi->pinctrl, PBL_PINCTRL_STATE_SLEEP);
  }
}

static int prv_init(const struct pbl_spi_bus *bus) {
  const struct pbl_spi_stm32 *spi = prv_spi(bus);
  int ret;

  ret = pbl_device_init(&spi->clk.ctrl->dev);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_pinctrl_select_state(&spi->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
  if (ret != 0) {
    return ret;
  }
  prv_idle_pins(spi);

  pbl_irq_enable(spi->irq);

  return 0;
}

// The divider: 2 to 256, the smallest that keeps the clock at or below the device's maximum
static int prv_configure(const struct pbl_spi_bus *bus, const struct pbl_spi_dev *dev) {
  const struct pbl_spi_stm32 *spi = prv_spi(bus);
  struct pbl_spi_stm32_state *state = spi->state;
  uint64_t rate_mhz;
  uint64_t hz;
  uint32_t mbr;
  int ret;

  ret = pbl_clk_get_rate(&spi->clk, &rate_mhz);
  if (ret != 0) {
    return ret;
  }

  hz = rate_mhz / MHZ_PER_HZ;
  for (mbr = 0U; mbr < 8U; mbr++) {
    if ((hz >> (mbr + 1U)) <= dev->max_hz) {
      break;
    }
  }
  if (mbr == 8U || dev->max_hz == 0U) {
    return -EINVAL;
  }

  state->cfg1 = PBL_FIELD_PREP(SPI_CFG1_MBR_MASK, mbr) | PBL_FIELD_PREP(SPI_CFG1_DSIZE_MASK, 7U) |
                PBL_FIELD_PREP(SPI_CFG1_FTHLV_MASK, 0U);
  state->cfg2 = SPI_CFG2_MASTER | SPI_CFG2_SSM | SPI_CFG2_AFCNTR;
  if ((dev->mode & PBL_SPI_CPHA) != 0U) {
    state->cfg2 |= SPI_CFG2_CPHA;
  }
  if ((dev->mode & PBL_SPI_CPOL) != 0U) {
    state->cfg2 |= SPI_CFG2_CPOL;
  }
  if ((dev->mode & PBL_SPI_LSB_FIRST) != 0U) {
    state->cfg2 |= SPI_CFG2_LSBFRST;
  }

  return 0;
}

// The configuration only takes writes with the controller disabled. Its clock stops in Stop: the
// CPU stays out of it until the bus is disabled.
static void prv_enable(const struct pbl_spi_bus *bus) {
  const struct pbl_spi_stm32 *spi = prv_spi(bus);

  pbl_idle_state_block(SOC_STM32_IDLE_STOP);
  (void)pbl_clk_enable(&spi->clk);
  prv_putreg(spi, STM32_SPI_CR1_OFFSET, SPI_CR1_SSI);
  prv_putreg(spi, STM32_SPI_CFG1_OFFSET, spi->state->cfg1);
  prv_putreg(spi, STM32_SPI_CFG2_OFFSET, spi->state->cfg2);
  (void)pbl_pinctrl_select_state(&spi->pinctrl, PBL_PINCTRL_STATE_DEFAULT);
}

static void prv_stop(const struct pbl_spi_stm32 *spi) {
  prv_putreg(spi, STM32_SPI_IER_OFFSET, 0U);
  prv_putreg(spi, STM32_SPI_CR1_OFFSET, SPI_CR1_SSI);
  prv_putreg(spi, STM32_SPI_IFCR_OFFSET, SPI_IFCR_ALL);
  pbl_irq_clear_pending(spi->irq);
}

static void prv_disable(const struct pbl_spi_bus *bus) {
  const struct pbl_spi_stm32 *spi = prv_spi(bus);

  prv_stop(spi);
  prv_idle_pins(spi);
  (void)pbl_clk_disable(&spi->clk);
  pbl_idle_state_release(SOC_STM32_IDLE_STOP);
}

static void prv_fill(const struct pbl_spi_stm32 *spi) {
  struct pbl_spi_stm32_state *state = spi->state;
  const struct pbl_spi_xfer *xfer = state->xfer;
  const uint8_t *tx = xfer->tx_buf;

  while (state->tx < xfer->len && state->tx - state->rx < TX_AHEAD &&
         (prv_getreg(spi, STM32_SPI_SR_OFFSET) & SPI_SR_TXP) != 0U) {
    pbl_writeb(tx != nullptr ? tx[state->tx] : 0U, spi->base + STM32_SPI_TXDR_OFFSET);
    state->tx++;
  }

  if (state->tx == xfer->len) {
    pbl_modifyl(SPI_IER_TXPIE, 0U, spi->base + STM32_SPI_IER_OFFSET);
  }
}

static void prv_drain(const struct pbl_spi_stm32 *spi) {
  struct pbl_spi_stm32_state *state = spi->state;
  const struct pbl_spi_xfer *xfer = state->xfer;
  uint8_t *rx = xfer->rx_buf;

  while (state->rx < xfer->len && (prv_getreg(spi, STM32_SPI_SR_OFFSET) & SPI_SR_RXP) != 0U) {
    uint8_t byte = pbl_readb(spi->base + STM32_SPI_RXDR_OFFSET);

    if (rx != nullptr) {
      rx[state->rx] = byte;
    }
    state->rx++;
  }
}

static int prv_start(const struct pbl_spi_bus *bus, const struct pbl_spi_xfer *xfer) {
  const struct pbl_spi_stm32 *spi = prv_spi(bus);
  struct pbl_spi_stm32_state *state = spi->state;

  if (xfer->len > SPI_TSIZE_MAX) {
    return -EINVAL;
  }

  state->xfer = xfer;
  state->tx = 0U;
  state->rx = 0U;

  prv_putreg(spi, STM32_SPI_CR1_OFFSET, SPI_CR1_SSI);
  prv_putreg(spi, STM32_SPI_IFCR_OFFSET, SPI_IFCR_ALL);
  prv_putreg(spi, STM32_SPI_CR2_OFFSET, PBL_FIELD_PREP(SPI_CR2_TSIZE_MASK, xfer->len));
  prv_putreg(spi, STM32_SPI_CR1_OFFSET, SPI_CR1_SSI | SPI_CR1_SPE);

  pbl_irq_lock();
  prv_putreg(spi, STM32_SPI_IER_OFFSET, IER_IRQS);
  prv_fill(spi);
  pbl_modifyl(0U, SPI_CR1_CSTART, spi->base + STM32_SPI_CR1_OFFSET);
  pbl_irq_unlock();

  return 0;
}

void pbl_spi_stm32_irq_handler(const struct pbl_spi_stm32 *spi) {
  struct pbl_spi_stm32_state *state = spi->state;
  uint32_t sr;

  if (prv_getreg(spi, STM32_SPI_IER_OFFSET) == 0U || state->xfer == nullptr) {
    return;
  }

  sr = prv_getreg(spi, STM32_SPI_SR_OFFSET);
  if ((sr & (SPI_SR_OVR | SPI_SR_MODF)) != 0U) {
    prv_stop(spi);
    state->xfer = nullptr;
    pbl_spi_bus_complete(&spi->bus, -EIO);
    return;
  }

  prv_drain(spi);
  prv_fill(spi);

  if (state->rx < state->xfer->len) {
    return;
  }

  for (uint32_t i = 0U; i < EOT_POLLS && (prv_getreg(spi, STM32_SPI_SR_OFFSET) & SPI_SR_EOT) == 0U;
       i++) {
  }

  prv_stop(spi);
  state->xfer = nullptr;
  pbl_spi_bus_complete(&spi->bus, 0);
}

const struct pbl_spi_bus_ops pbl_spi_stm32_ops = {
  .init = prv_init,
  .configure = prv_configure,
  .enable = prv_enable,
  .disable = prv_disable,
  .start = prv_start,
};

#include PBL_DT_INSTANCES(st_stm32h7_spi)
