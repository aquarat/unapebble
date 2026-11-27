/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <pbl/devicetree.h>
#include <pbl/drivers/dma/stm32u5.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/mmio.h>
#include <pbl/util/bits.h>
#include <pbl/util/misc.h>
#include <pbl/util/size.h>

#include <hardware/stm32u5_gpdma.h>

// A channel suspends once its current beat is done
#define SUSPEND_TIMEOUT_US 1000U

#define CCR_IRQS \
  (GPDMA_CCR_TCIE | GPDMA_CCR_HTIE | GPDMA_CCR_DTEIE | GPDMA_CCR_ULEIE | GPDMA_CCR_USEIE)

static const struct pbl_dma_stm32u5 *prv_dmac(const struct pbl_dma_controller *ctrl) {
  return PBL_CONTAINER_OF(ctrl, const struct pbl_dma_stm32u5, ctrl);
}

static uint32_t prv_getreg(const struct pbl_dma_stm32u5 *dmac, uint32_t offset) {
  return pbl_readl(dmac->base + offset);
}

static void prv_putreg(const struct pbl_dma_stm32u5 *dmac, uint32_t offset, uint32_t value) {
  pbl_writel(value, dmac->base + offset);
}

static bool prv_enabled(const struct pbl_dma_stm32u5 *dmac, uint8_t ch) {
  return (prv_getreg(dmac, STM32U5_GPDMA_CCR_OFFSET(ch)) & GPDMA_CCR_EN) != 0U;
}

static int prv_slave_config(const struct pbl_dma_controller *ctrl, const struct pbl_dma_chan *chan,
                            const struct pbl_dma_slave_config *cfg) {
  const struct pbl_dma_stm32u5 *dmac = prv_dmac(ctrl);
  struct pbl_dma_stm32u5_chan *state = &dmac->chans[chan->channel];

  if (chan->request > GPDMA_CTR2_REQSEL_MASK || chan->config > 3U) {
    return -EINVAL;
  }

  if (prv_enabled(dmac, chan->channel)) {
    return -EBUSY;
  }

  state->request = chan->request;
  state->prio = (uint8_t)chan->config;

  return 0;
}

// Stop a channel: suspended first, so that a beat in flight completes, then reset
static void prv_stop(const struct pbl_dma_stm32u5 *dmac, uint8_t ch) {
  pbl_irq_disable(dmac->irqs[ch]);

  if (prv_enabled(dmac, ch)) {
    pbl_modifyl(0U, GPDMA_CCR_SUSP, dmac->base + STM32U5_GPDMA_CCR_OFFSET(ch));
    (void)PBL_POLL_BUSY_US((prv_getreg(dmac, STM32U5_GPDMA_CSR_OFFSET(ch)) &
                            (GPDMA_CSR_SUSPF | GPDMA_CSR_IDLEF)) != 0U,
                           1U, SUSPEND_TIMEOUT_US);
  }

  prv_putreg(dmac, STM32U5_GPDMA_CCR_OFFSET(ch), GPDMA_CCR_RESET);
  prv_putreg(dmac, STM32U5_GPDMA_CFCR_OFFSET(ch), GPDMA_CSR_ALL);
  dmac->chans[ch].cyclic = false;
}

static int prv_start(const struct pbl_dma_controller *ctrl, uint8_t ch, void *buf, size_t len,
                     enum pbl_dma_dir dir, bool cyclic) {
  const struct pbl_dma_stm32u5 *dmac = prv_dmac(ctrl);
  const struct pbl_dma_chan_state *cstate = &ctrl->chans[ch];
  struct pbl_dma_stm32u5_chan *state = &dmac->chans[ch];
  bool to_mem = dir == PBL_DMA_DEV_TO_MEM;
  uint32_t width;
  uintptr_t dev;
  uint32_t log2w;
  uint32_t ccr;
  uint32_t ctr1;
  uint32_t ctr2;
  uint32_t cllr = 0U;

  if (prv_enabled(dmac, ch)) {
    return -EBUSY;
  }

  width = to_mem ? cstate->cfg.src_addr_width : cstate->cfg.dst_addr_width;
  dev = to_mem ? cstate->cfg.src_addr : cstate->cfg.dst_addr;
  log2w = width == 4U ? 2U : width - 1U;

  if (len == 0U || (len % width) != 0U || len > GPDMA_CBR1_BNDT_MASK ||
      ((uintptr_t)buf % width) != 0U) {
    return -EINVAL;
  }

  ctr1 = PBL_FIELD_PREP(GPDMA_CTR1_SDW_MASK, log2w) | PBL_FIELD_PREP(GPDMA_CTR1_DDW_MASK, log2w) |
         (to_mem ? GPDMA_CTR1_DINC : GPDMA_CTR1_SINC);
  ctr2 = PBL_FIELD_PREP(GPDMA_CTR2_REQSEL_MASK, state->request) | (to_mem ? 0U : GPDMA_CTR2_DREQ);

  // The block reloads its count, memory address and the link itself from the item, in that order
  if (cyclic) {
    uintptr_t lli = (uintptr_t)state->lli;

    cllr = GPDMA_CLLR_UB1 | (to_mem ? GPDMA_CLLR_UDA : GPDMA_CLLR_USA) | GPDMA_CLLR_ULL |
           (lli & GPDMA_CLLR_LA_MASK);
    state->lli[0] = (uint32_t)len;
    state->lli[1] = (uint32_t)(uintptr_t)buf;
    state->lli[2] = cllr;
    prv_putreg(dmac, STM32U5_GPDMA_CLBAR_OFFSET(ch), lli & GPDMA_CLBAR_LBA_MASK);
  }

  prv_putreg(dmac, STM32U5_GPDMA_CFCR_OFFSET(ch), GPDMA_CSR_ALL);
  prv_putreg(dmac, STM32U5_GPDMA_CTR1_OFFSET(ch), ctr1);
  prv_putreg(dmac, STM32U5_GPDMA_CTR2_OFFSET(ch), ctr2);
  prv_putreg(dmac, STM32U5_GPDMA_CBR1_OFFSET(ch), (uint32_t)len);
  prv_putreg(dmac, STM32U5_GPDMA_CSAR_OFFSET(ch),
             to_mem ? (uint32_t)dev : (uint32_t)(uintptr_t)buf);
  prv_putreg(dmac, STM32U5_GPDMA_CDAR_OFFSET(ch),
             to_mem ? (uint32_t)(uintptr_t)buf : (uint32_t)dev);
  prv_putreg(dmac, STM32U5_GPDMA_CLLR_OFFSET(ch), cllr);

  ccr = PBL_FIELD_PREP(GPDMA_CCR_PRIO_MASK, state->prio);
  if (cyclic) {
    ccr |= CCR_IRQS;
  } else if (cstate->callback != nullptr) {
    ccr |= CCR_IRQS & ~GPDMA_CCR_HTIE;
  }
  prv_putreg(dmac, STM32U5_GPDMA_CCR_OFFSET(ch), ccr);
  state->cyclic = cyclic;

  if ((ccr & CCR_IRQS) != 0U) {
    pbl_irq_clear_pending(dmac->irqs[ch]);
    pbl_irq_enable(dmac->irqs[ch]);
  }

  prv_putreg(dmac, STM32U5_GPDMA_CCR_OFFSET(ch), ccr | GPDMA_CCR_EN);

  return 0;
}

static int prv_prep_slave_single(const struct pbl_dma_controller *ctrl, uint8_t channel, void *buf,
                                 size_t len, enum pbl_dma_dir dir) {
  return prv_start(ctrl, channel, buf, len, dir, false);
}

// A block over the ring, its half and full transfer interrupts ending the two periods
static int prv_prep_dma_cyclic(const struct pbl_dma_controller *ctrl, uint8_t channel, void *buf,
                               size_t buf_len, size_t period_len, enum pbl_dma_dir dir) {
  if (buf_len != 2U * period_len) {
    return -EINVAL;
  }

  return prv_start(ctrl, channel, buf, buf_len, dir, true);
}

static int prv_terminate(const struct pbl_dma_controller *ctrl, uint8_t channel) {
  pbl_irq_lock();
  prv_stop(prv_dmac(ctrl), channel);
  pbl_irq_unlock();

  return 0;
}

static int prv_tx_status(const struct pbl_dma_controller *ctrl, uint8_t channel, size_t *residue) {
  const struct pbl_dma_stm32u5 *dmac = prv_dmac(ctrl);

  *residue =
      PBL_FIELD_GET(GPDMA_CBR1_BNDT_MASK, prv_getreg(dmac, STM32U5_GPDMA_CBR1_OFFSET(channel)));

  return 0;
}

void pbl_dma_stm32u5_irq_handler(const struct pbl_dma_stm32u5_chan_irq *irq) {
  const struct pbl_dma_stm32u5 *dmac = irq->dmac;
  const struct pbl_dma_controller *ctrl = &dmac->ctrl;
  uint8_t ch = irq->channel;
  uint32_t flags = prv_getreg(dmac, STM32U5_GPDMA_CSR_OFFSET(ch)) & GPDMA_CSR_ALL;

  if ((flags & GPDMA_CSR_ERRORS) != 0U) {
    prv_stop(dmac, ch);
    pbl_dma_complete(ctrl, ch, -EIO);
    return;
  }

  prv_putreg(dmac, STM32U5_GPDMA_CFCR_OFFSET(ch), flags);

  if (dmac->chans[ch].cyclic) {
    if ((flags & GPDMA_CSR_HTF) != 0U) {
      pbl_dma_complete(ctrl, ch, 0);
    }
    if ((flags & GPDMA_CSR_TCF) != 0U) {
      pbl_dma_complete(ctrl, ch, 0);
    }
    return;
  }

  if ((flags & GPDMA_CSR_TCF) == 0U) {
    return;
  }

  // The channel disabled itself at the end of its block
  pbl_irq_disable(dmac->irqs[ch]);
  pbl_dma_complete(ctrl, ch, 0);
}

int pbl_dma_stm32u5_init(const struct pbl_device *dev) {
  const struct pbl_dma_stm32u5 *dmac =
      PBL_CONTAINER_OF(dev, const struct pbl_dma_stm32u5, ctrl.dev);
  int ret;

  ret = pbl_device_init(&dmac->clk.ctrl->dev);
  if (ret != 0) {
    return ret;
  }

  ret = pbl_clk_enable(&dmac->clk);
  if (ret != 0) {
    return ret;
  }

  // The bootloader may have left channels running
  for (uint8_t ch = 0U; ch < PBL_DMA_STM32U5_NUM_CHANNELS; ch++) {
    prv_stop(dmac, ch);
  }

  return 0;
}

const struct pbl_dma_controller_ops pbl_dma_stm32u5_ops = {
  .slave_config = prv_slave_config,
  .prep_slave_single = prv_prep_slave_single,
  .prep_dma_cyclic = prv_prep_dma_cyclic,
  .terminate = prv_terminate,
  .tx_status = prv_tx_status,
};

#include PBL_DT_INSTANCES(st_stm32u5_gpdma)
