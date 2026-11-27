/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/clk.h>
#include <pbl/drivers/dma.h>
#include <pbl/kernel/irq.h>

/**
 * @defgroup drivers_dma_stm32u5 STM32U5 GPDMA
 * @ingroup drivers_dma
 * @brief @ref drivers_dma driver for the STM32U5 GPDMA1.
 *
 * Channels are the 16 channels of the controller, each fed by the request a consumer names in its
 * @c dmas entry, from the reference manual (@c STM32U5_GPDMA_REQ_* in
 * @c hardware/stm32u5_gpdma.h), with the channel priority, 0 to 3, as configuration:
 *
 * @code{.c}
 * static const struct pbl_dma_chan s_rx =
 *     PBL_DMA_CHAN(&s_gpdma1, 11, STM32U5_GPDMA_REQ_LPUART1_RX, 2);
 * @endcode
 *
 * Transfers are single beats on port 0, a block of up to 65535 bytes. A cyclic transfer runs a
 * ring of two periods: the block reloads itself from a linked-list item, and the half and full
 * transfer interrupts end the periods. Each channel has its own interrupt line, connected with
 * priority @c CONFIG_DMA_STM32U5_IRQ_PRIO and enabled while a transfer with a callback runs. The
 * controller does not run in Stop 2: a consumer that needs it then keeps the CPU out of Stop, or
 * wakes it up first.
 * @{
 */

/** @brief Channels. */
#define PBL_DMA_STM32U5_NUM_CHANNELS 16U

/** @brief Channel state. */
struct pbl_dma_stm32u5_chan {
  /** Request, from the slave configuration. */
  uint8_t request;
  /** Priority, 0 to 3. */
  uint8_t prio;
  /** A cyclic transfer runs. */
  bool cyclic;
  /** Linked-list item reloading the block of a cyclic transfer: CBR1, CSAR or CDAR, CLLR. */
  uint32_t lli[3];
};

/** @brief Controller. */
struct pbl_dma_stm32u5 {
  /** DMA controller. */
  struct pbl_dma_controller ctrl;
  /** Registers. */
  uintptr_t base;
  /** Clock. */
  struct pbl_clk clk;
  /** Interrupt line of each channel. */
  pbl_irq_t irqs[PBL_DMA_STM32U5_NUM_CHANNELS];
  /** State of each channel. */
  struct pbl_dma_stm32u5_chan *chans;
};

/** @cond INTERNAL_HIDDEN */
extern const struct pbl_dma_controller_ops pbl_dma_stm32u5_ops;
int pbl_dma_stm32u5_init(const struct pbl_device *dev);

/** @brief A channel of the controller, as its interrupt handler gets it. */
struct pbl_dma_stm32u5_chan_irq {
  /** Controller. */
  const struct pbl_dma_stm32u5 *dmac;
  /** Channel, from 0. */
  uint8_t channel;
};

void pbl_dma_stm32u5_irq_handler(const struct pbl_dma_stm32u5_chan_irq *irq);

#define PBL_DMA_STM32U5_CHAN_IRQ(sym, _c, _line)                                       \
  static const struct pbl_dma_stm32u5_chan_irq sym##_irq##_c = {&sym##_inst, _c};      \
  PBL_IRQ_CONNECT_NUM(_line, CONFIG_DMA_STM32U5_IRQ_PRIO, pbl_dma_stm32u5_irq_handler, \
                      &sym##_irq##_c, 0)
/** @endcond */

/**
 * @brief Define the controller.
 *
 * @param sym Symbol of the controller.
 * @param _name Name.
 * @param _base Registers.
 * @param _clk Clock, from PBL_CLK().
 * @param _i0 Interrupt line of channel 0, followed by those of channels 1 to 15.
 */
#define PBL_DMA_STM32U5_DEFINE(sym, _name, _base, _clk, _i0, _i1, _i2, _i3, _i4, _i5, _i6, _i7,   \
                               _i8, _i9, _i10, _i11, _i12, _i13, _i14, _i15)                      \
  PBL_DMA_CONTROLLER_STATE_DEFINE(sym, PBL_DMA_STM32U5_NUM_CHANNELS);                             \
  static struct pbl_dma_stm32u5_chan sym##_chans[PBL_DMA_STM32U5_NUM_CHANNELS];                   \
  PBL_DEVICE_INSTANCE(sym, struct pbl_dma_stm32u5, struct pbl_dma_controller, ctrl){              \
    .ctrl =                                                                                       \
        PBL_DMA_CONTROLLER_INIT(sym, _name, pbl_dma_stm32u5_init, nullptr, &pbl_dma_stm32u5_ops), \
    .base = (_base),                                                                              \
    .clk = _clk,                                                                                  \
    .irqs =                                                                                       \
        {_i0, _i1, _i2, _i3, _i4, _i5, _i6, _i7, _i8, _i9, _i10, _i11, _i12, _i13, _i14, _i15},   \
    .chans = sym##_chans,                                                                         \
  };                                                                                              \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 0, _i0);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 1, _i1);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 2, _i2);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 3, _i3);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 4, _i4);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 5, _i5);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 6, _i6);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 7, _i7);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 8, _i8);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 9, _i9);                                                          \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 10, _i10);                                                        \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 11, _i11);                                                        \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 12, _i12);                                                        \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 13, _i13);                                                        \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 14, _i14);                                                        \
  PBL_DMA_STM32U5_CHAN_IRQ(sym, 15, _i15)

/** @} */
