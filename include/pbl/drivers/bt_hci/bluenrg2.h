/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include <pbl/drivers/bt_hci.h>
#include <pbl/drivers/gpio.h>
#include <pbl/drivers/spi.h>

/**
 * @defgroup drivers_bt_hci_bluenrg2 BlueNRG-2
 * @ingroup drivers_bt_hci
 * @brief An ST BlueNRG-2 on SPI, running ST's DTM firmware, for @c NIMBLE_HCI_BLUENRG2.
 *
 * Its init holds the chip in reset with IRQ an input; the transport takes it from there. The
 * chip select is driven by the transport through the SPI device (pbl_spi_hold()), and stays an
 * input until it releases the reset.
 * @{
 */

/** @brief BlueNRG-2. */
struct pbl_bt_hci_bluenrg2 {
  /** Controller. */
  struct pbl_bt_hci hci;
  /** SPI device. */
  struct pbl_spi_dev spi;
  /** IRQ, active high. */
  struct pbl_gpio irq;
  /** RESETN, active low. */
  struct pbl_gpio reset;
};

/** @cond INTERNAL_HIDDEN */
int pbl_bt_hci_bluenrg2_init(const struct pbl_device *dev);
/** @endcond */

/**
 * @brief Define a BlueNRG-2.
 *
 * @param sym Symbol of the controller, a struct pbl_bt_hci.
 * @param _name Name.
 * @param _bus SPI bus.
 * @param _cs Chip select index.
 * @param _max_hz Maximum SPI clock frequency, in Hz.
 * @param _mode SPI mode flags.
 * @param _irq IRQ pin, from PBL_GPIO().
 * @param _reset RESETN pin, from PBL_GPIO().
 */
#define PBL_BT_HCI_BLUENRG2_DEFINE(sym, _name, _bus, _cs, _max_hz, _mode, _irq, _reset) \
  PBL_DEVICE_STATE_DEFINE(sym);                                                         \
  PBL_DEVICE_INSTANCE(sym, struct pbl_bt_hci_bluenrg2, struct pbl_bt_hci, hci) {        \
    .hci = PBL_BT_HCI_INIT(sym, _name, pbl_bt_hci_bluenrg2_init, &(_bus)->dev),         \
    .spi = PBL_SPI_DEV(_bus, _cs, _max_hz, _mode), .irq = _irq, .reset = _reset,        \
  }

/** @} */
