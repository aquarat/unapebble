/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

/* Register offsets, non-secure */

#define STM32U5_FLASH_ACR_OFFSET    0x0000
#define STM32U5_FLASH_NSKEYR_OFFSET 0x0008
#define STM32U5_FLASH_NSSR_OFFSET   0x0020
#define STM32U5_FLASH_NSCR_OFFSET   0x0028
#define STM32U5_FLASH_OPTR_OFFSET   0x0040

/* Register bit definitions */

#define FLASH_ACR_LATENCY_MASK PBL_GENMASK(3, 0)
#define FLASH_ACR_LATENCY_MAX  15U

#define FLASH_NSKEYR_KEY1 0x45670123U
#define FLASH_NSKEYR_KEY2 0xcdef89abU

#define FLASH_NSSR_EOP     PBL_BIT(0)
#define FLASH_NSSR_OPERR   PBL_BIT(1)
#define FLASH_NSSR_PROGERR PBL_BIT(3)
#define FLASH_NSSR_WRPERR  PBL_BIT(4)
#define FLASH_NSSR_PGAERR  PBL_BIT(5)
#define FLASH_NSSR_SIZERR  PBL_BIT(6)
#define FLASH_NSSR_PGSERR  PBL_BIT(7)
#define FLASH_NSSR_OPTWERR PBL_BIT(13)
#define FLASH_NSSR_BSY     PBL_BIT(16)
#define FLASH_NSSR_WDW     PBL_BIT(17)
#define FLASH_NSSR_ERRORS                                                                    \
  (FLASH_NSSR_OPERR | FLASH_NSSR_PROGERR | FLASH_NSSR_WRPERR | FLASH_NSSR_PGAERR |            \
   FLASH_NSSR_SIZERR | FLASH_NSSR_PGSERR | FLASH_NSSR_OPTWERR)

#define FLASH_NSCR_PG       PBL_BIT(0)
#define FLASH_NSCR_PER      PBL_BIT(1)
#define FLASH_NSCR_PNB_MASK PBL_GENMASK(10, 3)
#define FLASH_NSCR_BKER     PBL_BIT(11)
#define FLASH_NSCR_STRT     PBL_BIT(16)
#define FLASH_NSCR_LOCK     PBL_BIT(31)

#define FLASH_OPTR_SWAP_BANK PBL_BIT(20)

/* Page size of the 2 and 4 MiB parts, in both banks */
#define STM32U5_FLASH_PAGE_SIZE 0x2000U

/* Flash size in KiB, in system memory */
#define STM32U5_FLASHSIZE_ADDR 0x0bfa07a0U
