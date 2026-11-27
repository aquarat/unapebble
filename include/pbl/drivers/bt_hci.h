/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/device.h>

/**
 * @defgroup drivers_bt_hci Bluetooth HCI controllers
 * @ingroup drivers
 * @brief Bluetooth controllers on a bus, as described for the HCI transport that drives them.
 *
 * A device of this class only gives the transport the controller's bus and pins; the transport,
 * in @c third_party/nimble/transport, speaks the controller's protocol. The board's controller is
 * the @c bt_hci role, @ref BT_HCI.
 * @{
 */

/** @brief Class of the Bluetooth HCI controllers. */
extern const struct pbl_device_class pbl_bt_hci_class;

/** @brief A Bluetooth HCI controller. */
struct pbl_bt_hci {
  /** Device. */
  struct pbl_device dev;
};

/** @brief The board's Bluetooth controller, for its HCI transport. */
extern const struct pbl_bt_hci *const BT_HCI;

/**
 * @brief Initializer for a struct pbl_bt_hci.
 *
 * @param sym Symbol of the instance, with its state defined by PBL_DEVICE_STATE_DEFINE().
 * @param _name Name.
 * @param _init Device init function, or NULL.
 * @param _parent Parent device, or NULL.
 */
#define PBL_BT_HCI_INIT(sym, _name, _init, _parent) \
  {.dev = PBL_DEVICE_INIT_CLASS(sym, _name, _init, _parent, &pbl_bt_hci_class)}

/** @} */
