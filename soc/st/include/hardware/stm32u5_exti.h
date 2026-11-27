/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

/* Register offsets: lines 0 to 15 are the GPIO lines, the port of each in EXTICR */

#define STM32U5_EXTI_RTSR1_OFFSET        0x0000
#define STM32U5_EXTI_FTSR1_OFFSET        0x0004
#define STM32U5_EXTI_RPR1_OFFSET         0x000c
#define STM32U5_EXTI_FPR1_OFFSET         0x0010
#define STM32U5_EXTI_EXTICR_OFFSET(line) (0x0060 + ((line) / 4U) * 4U)
#define STM32U5_EXTI_IMR1_OFFSET         0x0080
#define STM32U5_EXTI_EMR1_OFFSET         0x0084

/* Register bit definitions */

#define EXTI_EXTICR_MASK(line) PBL_GENMASK(((line) % 4U) * 8U + 7U, ((line) % 4U) * 8U)
