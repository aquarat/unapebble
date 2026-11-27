/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/board.h>
#include <pbl/device.h>
#include <pbl/drivers/gpio.h>
#include <pbl/kernel/delay.h>
#include <pbl/kernel/reboot.h>
#include <pbl/kernel/work.h>
#include <pbl/logging/logging.h>
#include <pbl/util/size.h>

#include <devicetree/devices.h>

// The enables of the loads the board switches, off at boot as the stock firmware leaves them:
// eMMC, buzzer, barometer LDO (which must not float), BLE reset and GNSS. The haptics' is its
// driver's, and so are the eMMC's and the GNSS's with their drivers: the board comes up after the
// devices, by when they may be in use.
static const struct pbl_gpio s_off_at_boot[] = {
#ifndef CONFIG_MMC_STM32_SDMMC
  PBL_GPIO(&dt_gpiod, 10, 0), /* EMMC_EN */
#endif
  PBL_GPIO(&dt_gpiob, 8, 0), /* BUZZER_PWM */
  PBL_GPIO(&dt_gpiob, 9, 0), /* BUZZER_EN1 */
  PBL_GPIO(&dt_gpiod, 5, 0), /* BUZZER_EN2 */
  PBL_GPIO(&dt_gpiog, 0, 0), /* ALTIMETER_EN */
  PBL_GPIO(&dt_gpioa, 8, 0), /* SENSOR1_RST_L, BLE in reset */
#ifndef CONFIG_GNSS_AG3335
  PBL_GPIO(&dt_gpiog, 5, 0), /* GNSS_EN */
#endif
};

#ifdef CONFIG_BOARD_UNAWATCH_CONSOLE_DAM
// USB_DAM, the switch of the debug accessory lines of the USB-C connector
static const struct pbl_gpio s_usb_dam = PBL_GPIO(&dt_gpiob, 15, 0);
#endif

// The power button, SW2, which the bootloader checks when the watch was powered off
static const struct pbl_gpio s_power_button = PBL_GPIO(&dt_gpiod, 15, PBL_GPIO_ACTIVE_LOW);

#define POWER_BUTTON_RELEASE_TIMEOUT_MS 30000U

struct board {
  struct pbl_device dev;
};

#if defined(CONFIG_BOARD_UNAWATCH_TETHER_REBOOT_S) && CONFIG_BOARD_UNAWATCH_TETHER_REBOOT_S > 0
#define TETHER_REBOOT_S CONFIG_BOARD_UNAWATCH_TETHER_REBOOT_S

PBL_LOG_MODULE_DEFINE(board_unawatch, LOG_LEVEL_INFO);

// Back to the tether stub once a test run is over, from the high priority work queue, which a
// stuck background queue does not hold up
static void prv_tether_reboot(struct pbl_work *work) {
  PBL_LOG_INFO("Restarting into the tether stub after %d s", TETHER_REBOOT_S);
  pbl_reboot();
}

static PBL_DELAYED_WORK_DEFINE(s_tether_reboot, prv_tether_reboot);
#endif

static int prv_init(const struct pbl_device *dev) {
  int ret;

  for (size_t i = 0U; i < PBL_ARRAY_SIZE(s_off_at_boot); i++) {
    ret = pbl_device_init(&s_off_at_boot[i].port->dev);
    if (ret == 0) {
      ret = pbl_gpio_configure(&s_off_at_boot[i], PBL_GPIO_OUTPUT_LOW);
    }
    if (ret != 0) {
      return ret;
    }
  }

#ifdef CONFIG_BOARD_UNAWATCH_CONSOLE_DAM
  ret = pbl_device_init(&s_usb_dam.port->dev);
  if (ret == 0) {
    ret = pbl_gpio_configure(&s_usb_dam, PBL_GPIO_OUTPUT_HIGH);
  }
  if (ret != 0) {
    return ret;
  }
#endif

#ifdef TETHER_REBOOT_S
  (void)pbl_delayed_work_schedule(&pbl_sys_workq_high, &s_tether_reboot, PBL_SEC(TETHER_REBOOT_S));
#endif

  return 0;
}

PBL_DEVICE_STATE_DEFINE(s_board);
PBL_DEVICE_INSTANCE_DEV(s_board, struct board){
  .dev = PBL_DEVICE_INIT(s_board, "board", prv_init, nullptr),
};

// The bootloader powers the watch off again after the reset of power-off, unless the power button
// is held: the press that asked for it has to end first
static void prv_power_off_prepare(enum pbl_sys_off_mode mode, void *user_data) {
  if (pbl_device_init(&s_power_button.port->dev) != 0 ||
      pbl_gpio_configure(&s_power_button, PBL_GPIO_INPUT | PBL_GPIO_PULL_UP) != 0) {
    return;
  }

  (void)PBL_POLL_SLEEP_MS(pbl_gpio_get(&s_power_button) == 0, 10U, POWER_BUTTON_RELEASE_TIMEOUT_MS);
}

PBL_SYS_OFF_HANDLER_DEFINE(unawatch_power_button, PBL_SYS_OFF_POWER_OFF_PREPARE,
                           PBL_SYS_OFF_PRIO_LOW, prv_power_off_prepare, nullptr);
