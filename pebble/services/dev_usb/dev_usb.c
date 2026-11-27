/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

// The USB developer access: the setting, the USB following it and the charger's input, and the
// reboot commands of the shell

#include "dev_usb_private.h"

#include <stdatomic.h>
#include <string.h>

#include <pbl/kernel/delay.h>
#include <pbl/kernel/reboot.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/services/battery/battery_charger.h>
#include <pbl/services/dev_usb.h>
#include <pbl/settings/settings.h>
#include <pbl/usb/cdc_acm.h>
#include <pbl/usb/usbd.h>

#ifdef CONFIG_SHELL
#include <pbl/shell/shell.h>
#endif

#include <applib/event_service_client.h>
#include <system/reboot_reason.h>

PBL_LOG_MODULE_DEFINE(dev_usb, CONFIG_DEV_USB_LOG_LEVEL);

// A settings file of its own: the preferences a phone syncs cannot turn it on
#define SETTINGS_FILE     "devusb"
#define SETTINGS_FILE_LEN 512
#define SETTINGS_KEY      "enabled"

// What the console sends before a restart has this long to leave
#define FLUSH_TIMEOUT_MS 200U

static atomic_bool s_enabled;
static atomic_bool s_plugged;
static atomic_bool s_ready;
static atomic_bool s_held_off;
static EventServiceInfo s_battery_event_info;

static bool prv_load(void) {
  struct pbl_settings_file file = {};
  bool enabled = false;

  if (pbl_settings_open(&file, SETTINGS_FILE, SETTINGS_FILE_LEN) != 0) {
    return false;
  }
  if (pbl_settings_get_len(&file, SETTINGS_KEY, sizeof(SETTINGS_KEY)) == sizeof(enabled)) {
    (void)pbl_settings_get(&file, SETTINGS_KEY, sizeof(SETTINGS_KEY), &enabled, sizeof(enabled));
  }
  pbl_settings_close(&file);

  return enabled;
}

static void prv_store(bool enabled) {
  struct pbl_settings_file file = {};

  if (pbl_settings_open(&file, SETTINGS_FILE, SETTINGS_FILE_LEN) != 0) {
    PBL_LOG_ERR("Cannot keep the setting");
    return;
  }
  (void)pbl_settings_set(&file, SETTINGS_KEY, sizeof(SETTINGS_KEY), &enabled, sizeof(enabled));
  pbl_settings_close(&file);
}

bool dev_usb_is_forced(void) {
#ifdef CONFIG_DEV_USB_FORCE
  return true;
#else
  return false;
#endif
}

static bool prv_wanted(void) {
  if (atomic_load(&s_held_off)) {
    return false;
  }
  if (dev_usb_is_forced()) {
    return true;
  }
  if (!atomic_load(&s_enabled)) {
    return false;
  }
#ifdef CONFIG_DEV_USB_FOLLOW_VBUS
  return atomic_load(&s_plugged);
#else
  return true;
#endif
}

// On the system work queue, so that connecting and disconnecting never race
static void prv_apply(void *data) {
  const bool want = prv_wanted();
  int ret;

  if (!atomic_load(&s_ready) || want == pbl_usbd_is_enabled()) {
    return;
  }

  if (want) {
    ret = pbl_usbd_enable();
    if (ret != 0) {
      PBL_LOG_ERR("USB failed to come up (%d)", ret);
      return;
    }
#ifdef CONFIG_DEV_USB_TETHER
    dev_usb_tether_start();
#endif
    PBL_LOG_INFO("USB developer access on");
  } else {
    (void)pbl_usbd_disable();
    PBL_LOG_INFO("USB developer access off");
  }
}

static void prv_schedule(void) {
  (void)pbl_sys_work_call(prv_apply, nullptr);
}

static void prv_battery_connection(PebbleEvent *e, void *context) {
  atomic_store(&s_plugged, e->battery_connection.is_connected);
  prv_schedule();
}

void dev_usb_init(void) {
  atomic_store(&s_enabled, prv_load());
  atomic_store(&s_plugged, battery_is_usb_connected());
  atomic_store(&s_ready, true);

  s_battery_event_info = (EventServiceInfo){
    .type = PEBBLE_BATTERY_CONNECTION_EVENT,
    .handler = prv_battery_connection,
  };
  event_service_client_subscribe(&s_battery_event_info);

  prv_schedule();
}

bool dev_usb_get_enabled(void) {
  return atomic_load(&s_enabled);
}

void dev_usb_set_enabled(bool enabled) {
  atomic_store(&s_enabled, enabled);
  prv_store(enabled);
  prv_schedule();
}

bool dev_usb_is_connected(void) {
  return pbl_usbd_is_enabled();
}

void dev_usb_hold_off(bool hold) {
  atomic_store(&s_held_off, hold);
  prv_schedule();
}

// The host sees the device go before the restart, and gets what the console still had to send
static void prv_sys_off(enum pbl_sys_off_mode mode, void *user_data) {
  if (!pbl_usbd_is_enabled()) {
    return;
  }

  (void)PBL_POLL_BUSY_US(pbl_usb_cdc_acm_tx_idle(USB_CDC_ACM_UART(0)), 1000U,
                         FLUSH_TIMEOUT_MS * 1000U);
  (void)pbl_usbd_disable();
}

PBL_SYS_OFF_HANDLER_DEFINE(dev_usb_restart, PBL_SYS_OFF_RESTART_PREPARE, PBL_SYS_OFF_PRIO_LOW,
                           prv_sys_off, nullptr);
PBL_SYS_OFF_HANDLER_DEFINE(dev_usb_power_off, PBL_SYS_OFF_POWER_OFF_PREPARE, PBL_SYS_OFF_PRIO_LOW,
                           prv_sys_off, nullptr);

void dev_usb_reboot(void) {
  RebootReason reason = {RebootReasonCode_Serial, 0};

  reboot_reason_set(&reason);
  pbl_reboot();
}

void dev_usb_power_off(void) {
  pbl_power_off();
}

#ifdef CONFIG_SHELL
static int prv_cmd_reboot(const struct pbl_shell *sh, size_t argc, char **argv) {
  if (argc < 2) {
#ifdef CONFIG_BOARD_UNAWATCH_TETHER_SLOT
    pbl_shell_print(sh, "Restarting: the bootloader starts the tether stub");
#else
    pbl_shell_print(sh, "Restarting");
#endif
    dev_usb_reboot();
  }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "menu") == 0) {
    // The bootloader has no flag for its menu: a power-on with the button held 4 s is the way in
    pbl_shell_print(
        sh, "Powering off%s",
        strcmp(argv[1], "menu") == 0 ? ": hold the power button 4 s to get the menu" : "");
    dev_usb_power_off();
  }

  pbl_shell_error(sh, "expected nothing, off or menu");
  return -1;
}

PBL_SHELL_CMD_ARG_REGISTER(reboot, nullptr,
                           "Restart (to the tether stub in a slot build) [off|menu]: power off",
                           prv_cmd_reboot, 1, 1);

static int prv_cmd_usb(const struct pbl_shell *sh, size_t argc, char **argv) {
  if (argc > 1) {
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
      dev_usb_set_enabled(strcmp(argv[1], "on") == 0);
    } else {
      pbl_shell_error(sh, "expected on or off");
      return -1;
    }
  }

  pbl_shell_print(sh, "setting %s%s, %s, USB %s%s", atomic_load(&s_enabled) ? "on" : "off",
                  dev_usb_is_forced() ? " (forced on)" : "",
                  atomic_load(&s_plugged) ? "plugged in" : "unplugged",
                  pbl_usbd_is_enabled() ? "connected" : "off",
                  pbl_usbd_is_configured()
                      ? (pbl_usbd_bulk_mps() == 512U ? ", configured at high speed"
                                                     : ", configured at full speed")
                      : "");
  return 0;
}

PBL_SHELL_CMD_ARG_REGISTER(usb, nullptr, "USB developer access [on|off]", prv_cmd_usb, 1, 1);
#endif
