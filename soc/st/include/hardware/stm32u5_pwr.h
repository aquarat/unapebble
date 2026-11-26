/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

/* Register offsets */

#define STM32U5_PWR_CR1_OFFSET   0x0000
#define STM32U5_PWR_CR3_OFFSET   0x0008
#define STM32U5_PWR_VOSR_OFFSET  0x000c
#define STM32U5_PWR_SVMCR_OFFSET 0x0010
#define STM32U5_PWR_DBPR_OFFSET  0x0028
#define STM32U5_PWR_SR_OFFSET    0x0038
#define STM32U5_PWR_SVMSR_OFFSET 0x003c

/* Register bit definitions */

#define PWR_CR1_LPMS_MASK  PBL_GENMASK(2, 0)
#define PWR_CR1_LPMS_STOP0 0U
#define PWR_CR1_LPMS_STOP1 1U
#define PWR_CR1_LPMS_STOP2 2U

/* The core supply: the LDO at reset, or the SMPS */
#define PWR_CR3_REGSEL PBL_BIT(1)

/* Ranges 1 (VOS = 3, up to 160 MHz) to 4 (VOS = 0, up to 25 MHz); the EPOD booster is needed
   above 55 MHz, in ranges 1 and 2 */
#define PWR_VOSR_BOOSTRDY PBL_BIT(14)
#define PWR_VOSR_VOSRDY   PBL_BIT(15)
#define PWR_VOSR_VOS_MASK PBL_GENMASK(17, 16)
#define PWR_VOSR_VOS_1    3U
#define PWR_VOSR_BOOSTEN  PBL_BIT(18)

/* VDDIO2 (port G) taken as valid */
#define PWR_SVMCR_IO2SV PBL_BIT(29)

/* Write access to the backup domain: RCC BDCR, RTC and TAMP */
#define PWR_DBPR_DBP PBL_BIT(0)

#define PWR_SVMSR_REGS        PBL_BIT(1)
#define PWR_SVMSR_ACTVOSRDY   PBL_BIT(15)
#define PWR_SVMSR_ACTVOS_MASK PBL_GENMASK(17, 16)
