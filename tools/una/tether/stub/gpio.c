/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include "regs.h"
#include "stub.h"

#define GPIOR(port, off) (GPIO_BASE(port) + (off))

void gpio_clocks_on(void) {
  /* Ports A to I */
  setb(RCC(STM32U5_RCC_AHB2ENR1_OFFSET), 0x1ffU);
  (void)rd(RCC(STM32U5_RCC_AHB2ENR1_OFFSET));
}

void gpio_config(unsigned port, unsigned pin, unsigned mode, bool open_drain, unsigned pull,
                 unsigned af) {
  modb(GPIOR(port, STM32_GPIO_AFR_OFFSET(pin)), GPIO_AFR_MASK(pin),
       (af & 0xfU) << ((pin % 8U) * 4U));
  modb(GPIOR(port, STM32_GPIO_OTYPER_OFFSET), 1U << pin, open_drain ? (1U << pin) : 0U);
  /* Very high speed: the display's data and clocks need their edges */
  modb(GPIOR(port, STM32_GPIO_OSPEEDR_OFFSET), GPIO_OSPEEDR_MASK(pin), 3U << (pin * 2U));
  modb(GPIOR(port, STM32_GPIO_PUPDR_OFFSET), GPIO_PUPDR_MASK(pin), pull << (pin * 2U));
  modb(GPIOR(port, STM32_GPIO_MODER_OFFSET), GPIO_MODER_MASK(pin), mode << (pin * 2U));
}

void gpio_write(unsigned port, unsigned pin, bool level) {
  wr(GPIOR(port, STM32_GPIO_BSRR_OFFSET), level ? GPIO_BSRR_SET(pin) : GPIO_BSRR_RESET(pin));
}

bool gpio_read(unsigned port, unsigned pin) {
  return (rd(GPIOR(port, STM32_GPIO_IDR_OFFSET)) & (1U << pin)) != 0U;
}

static void prv_out(unsigned port, unsigned pin, bool level) {
  gpio_write(port, pin, level);
  gpio_config(port, pin, GPIO_OUT, false, PULL_NONE, 0U);
}

/*
 * The loads off, as the stock kernel and PebbleOS leave them at boot: eMMC,
 * buzzer, barometer LDO (which must not float), BLE in reset, GNSS, haptics,
 * and the debug accessory switch of the USB-C connector, which the stock
 * kernel only closes for a debug accessory.
 */
void gpio_pins_init(void) {
  prv_out(PORT_D, 10, false); /* EMMC_EN */
  prv_out(PORT_B, 8, false);  /* BUZZER_PWM */
  prv_out(PORT_B, 9, false);  /* BUZZER_EN1 */
  prv_out(PORT_D, 5, false);  /* BUZZER_EN2 */
  prv_out(PORT_G, 0, false);  /* ALTIMETER_EN */
  prv_out(PORT_A, 8, false);  /* SENSOR1_RST_L: BLE in reset */
  prv_out(PORT_G, 5, false);  /* GNSS_EN */
  prv_out(PORT_F, 2, false);  /* HAPTIC_RST_N */
  prv_out(PORT_B, 15, false); /* USB_DAM */

  /* Buttons: active low, on the internal pull-ups */
  gpio_config(PORT_D, 8, GPIO_IN, false, PULL_UP, 0U);  /* Back */
  gpio_config(PORT_D, 4, GPIO_IN, false, PULL_UP, 0U);  /* Up */
  gpio_config(PORT_D, 15, GPIO_IN, false, PULL_UP, 0U); /* Select, the power button */
  gpio_config(PORT_A, 5, GPIO_IN, false, PULL_UP, 0U);  /* Down */

  /* The frontlight, open drain, on low */
  gpio_write(PORT_F, 3, false);
  gpio_config(PORT_F, 3, GPIO_OUT, true, PULL_NONE, 0U);

  /* USB D- and D+ belong to the PHY */
  gpio_config(PORT_A, 11, GPIO_ANALOG, false, PULL_NONE, 0U);
  gpio_config(PORT_A, 12, GPIO_ANALOG, false, PULL_NONE, 0U);
}

/* Bit 0 back, 1 up, 2 select, 3 down, set when pressed */
uint8_t buttons_read(void) {
  uint8_t b = 0U;

  b |= gpio_read(PORT_D, 8) ? 0U : 1U;
  b |= gpio_read(PORT_D, 4) ? 0U : 2U;
  b |= gpio_read(PORT_D, 15) ? 0U : 4U;
  b |= gpio_read(PORT_A, 5) ? 0U : 8U;
  return b;
}
