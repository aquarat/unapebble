/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

/* Register offsets */

#define STM32U5_ICACHE_CR_OFFSET 0x0000
#define STM32U5_ICACHE_SR_OFFSET 0x0004

/* Register bit definitions */

#define ICACHE_CR_EN       PBL_BIT(0)
#define ICACHE_CR_CACHEINV PBL_BIT(1)
#define ICACHE_CR_WAYSEL   PBL_BIT(2)

#define ICACHE_SR_BUSYF PBL_BIT(0)
