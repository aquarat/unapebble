/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

#define STM32_RNG_CR_OFFSET 0x0000
#define STM32_RNG_SR_OFFSET 0x0004
#define STM32_RNG_DR_OFFSET 0x0008
/* Health test configuration, on the RNG with conditioning (STM32U5) */
#define STM32_RNG_HTCR_OFFSET 0x0010

#define RNG_CR_RNGEN PBL_BIT(2)
#define RNG_CR_CED   PBL_BIT(5)
/* The conditioning configuration, RNG_CONFIG1 to 3, NISTC and CLKDIV, taken while CONDRST is set */
#define RNG_CR_CONFIG_MASK (PBL_GENMASK(25, 8))
#define RNG_CR_CONDRST     PBL_BIT(30)

#define RNG_SR_DRDY PBL_BIT(0)
#define RNG_SR_CECS PBL_BIT(1)
#define RNG_SR_SECS PBL_BIT(2)
#define RNG_SR_CEIS PBL_BIT(5)
#define RNG_SR_SEIS PBL_BIT(6)
