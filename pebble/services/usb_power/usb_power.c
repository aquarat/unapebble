/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdatomic.h>

#include <pbl/kernel/reboot.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/services/usb_power.h>
#include <pbl/usb/usbd.h>

PBL_LOG_MODULE_DEFINE(usb_power, CONFIG_SERVICE_USB_POWER_LOG_LEVEL);

static atomic_uint s_users;
static atomic_bool s_held_off;

// On the system work queue, so that connecting and disconnecting never race
static void prv_apply(void *data) {
  const bool want = atomic_load(&s_users) != 0U && !atomic_load(&s_held_off);
  int ret;

  if (want == pbl_usbd_is_enabled()) {
    return;
  }

  if (want) {
    ret = pbl_usbd_enable();
    if (ret != 0) {
      PBL_LOG_ERR("USB failed to come up (%d)", ret);
      return;
    }
    PBL_LOG_DBG("USB on, for 0x%x", atomic_load(&s_users));
  } else {
    (void)pbl_usbd_disable();
    PBL_LOG_DBG("USB off");
  }
}

void usb_power_request(enum usb_power_user user, bool on) {
  if (on) {
    atomic_fetch_or(&s_users, (unsigned int)user);
  } else {
    atomic_fetch_and(&s_users, ~(unsigned int)user);
  }
  (void)pbl_sys_work_call(prv_apply, nullptr);
}

void usb_power_hold_off(bool hold) {
  atomic_store(&s_held_off, hold);
  (void)pbl_sys_work_call(prv_apply, nullptr);
}

bool usb_power_is_on(void) {
  return pbl_usbd_is_enabled();
}

#ifndef CONFIG_DEV_USB
// The host sees the device go before a restart or a power-off; the developer access does so
// itself, once the console has sent what it had
static void prv_sys_off(enum pbl_sys_off_mode mode, void *user_data) {
  (void)pbl_usbd_disable();
}

PBL_SYS_OFF_HANDLER_DEFINE(usb_power_restart, PBL_SYS_OFF_RESTART_PREPARE, PBL_SYS_OFF_PRIO_LOW,
                           prv_sys_off, nullptr);
PBL_SYS_OFF_HANDLER_DEFINE(usb_power_power_off, PBL_SYS_OFF_POWER_OFF_PREPARE, PBL_SYS_OFF_PRIO_LOW,
                           prv_sys_off, nullptr);
#endif
