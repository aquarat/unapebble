/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_CLOCK_ST_STM32U5_RCC_H
#define _DT_BINDINGS_CLOCK_ST_STM32U5_RCC_H

/* STM32U5 RCC clocks */

/* Oscillators */
#define STM32U5_CLK_HSE   0
#define STM32U5_CLK_LSE   1
#define STM32U5_CLK_HSI16 2
#define STM32U5_CLK_HSI48 3
#define STM32U5_CLK_MSIS  4
#define STM32U5_CLK_MSIK  5
#define STM32U5_CLK_LSI   6

/* PLL1: its source (MSIS, HSI16 or HSE), and its outputs */
#define STM32U5_CLK_PLL1_SRC 7
#define STM32U5_CLK_PLL1_P   8
#define STM32U5_CLK_PLL1_Q   9
#define STM32U5_CLK_PLL1_R   10

/* System clock (MSIS, HSI16, HSE or PLL1 R) and bus clocks */
#define STM32U5_CLK_SYSCLK 11
#define STM32U5_CLK_HCLK   12
#define STM32U5_CLK_PCLK1  13
#define STM32U5_CLK_PCLK2  14
#define STM32U5_CLK_PCLK3  15

/* Peripheral clocks, with the kernel clock of those that have one */
#define STM32U5_CLK_GPIOA   16
#define STM32U5_CLK_GPIOB   17
#define STM32U5_CLK_GPIOC   18
#define STM32U5_CLK_GPIOD   19
#define STM32U5_CLK_GPIOE   20
#define STM32U5_CLK_GPIOF   21
#define STM32U5_CLK_GPIOG   22
#define STM32U5_CLK_GPIOH   23
#define STM32U5_CLK_GPIOI   24
#define STM32U5_CLK_GPDMA1  25
#define STM32U5_CLK_CRC     26
#define STM32U5_CLK_FLASH   27
#define STM32U5_CLK_RNG     28
#define STM32U5_CLK_OTG     29
#define STM32U5_CLK_USBPHY  30
#define STM32U5_CLK_SDMMC1  31
#define STM32U5_CLK_PWR     32
#define STM32U5_CLK_USART1  33
#define STM32U5_CLK_USART2  34
#define STM32U5_CLK_USART3  35
#define STM32U5_CLK_LPUART1 36
#define STM32U5_CLK_I2C1    37
#define STM32U5_CLK_I2C2    38
#define STM32U5_CLK_I2C3    39
#define STM32U5_CLK_I2C4    40
#define STM32U5_CLK_SPI1    41
#define STM32U5_CLK_SPI2    42
#define STM32U5_CLK_SPI3    43
#define STM32U5_CLK_LPTIM1  44
#define STM32U5_CLK_LPTIM2  45
#define STM32U5_CLK_LPTIM3  46
#define STM32U5_CLK_LPTIM4  47
#define STM32U5_CLK_RTCAPB  48
#define STM32U5_CLK_RTC     49
#define STM32U5_CLK_SYSCFG  50
#define STM32U5_CLK_WWDG    51
/* ICLK, the 48 MHz kernel clock of USB and SDMMC: HSI48, PLL2 Q, PLL1 Q or MSIK */
#define STM32U5_CLK_ICLK 52

#endif /* _DT_BINDINGS_CLOCK_ST_STM32U5_RCC_H */
