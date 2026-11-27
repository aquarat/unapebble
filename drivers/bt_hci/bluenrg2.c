/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/devicetree.h>
#include <pbl/drivers/bt_hci/bluenrg2.h>
#include <pbl/util/misc.h>

// In reset, the chip select undriven, until the transport starts the chip
int pbl_bt_hci_bluenrg2_init(const struct pbl_device *dev) {
  const struct pbl_bt_hci_bluenrg2 *nrg =
      PBL_CONTAINER_OF(dev, const struct pbl_bt_hci_bluenrg2, hci.dev);
  int ret;

  ret = pbl_device_init(&nrg->reset.port->dev);
  if (ret == 0) {
    ret = pbl_gpio_configure(&nrg->reset, PBL_GPIO_OUTPUT_ACTIVE);
  }
  if (ret == 0) {
    ret = pbl_device_init(&nrg->irq.port->dev);
  }
  if (ret == 0) {
    ret = pbl_gpio_configure(&nrg->irq, PBL_GPIO_INPUT);
  }

  return ret;
}

#include PBL_DT_INSTANCES(st_bluenrg_2)
