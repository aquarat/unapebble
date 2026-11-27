/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

/* Registers of a channel, from the start of the controller */

#define STM32U5_GPDMA_CH_OFFSET(ch)    (0x0050U + 0x80U * (ch))
#define STM32U5_GPDMA_CLBAR_OFFSET(ch) (STM32U5_GPDMA_CH_OFFSET(ch) + 0x00U)
#define STM32U5_GPDMA_CFCR_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x0cU)
#define STM32U5_GPDMA_CSR_OFFSET(ch)   (STM32U5_GPDMA_CH_OFFSET(ch) + 0x10U)
#define STM32U5_GPDMA_CCR_OFFSET(ch)   (STM32U5_GPDMA_CH_OFFSET(ch) + 0x14U)
#define STM32U5_GPDMA_CTR1_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x40U)
#define STM32U5_GPDMA_CTR2_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x44U)
#define STM32U5_GPDMA_CBR1_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x48U)
#define STM32U5_GPDMA_CSAR_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x4cU)
#define STM32U5_GPDMA_CDAR_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x50U)
#define STM32U5_GPDMA_CLLR_OFFSET(ch)  (STM32U5_GPDMA_CH_OFFSET(ch) + 0x7cU)

/* Flags, in CSR, cleared by the same bits in CFCR and enabled by them in CCR */
#define GPDMA_CSR_IDLEF  PBL_BIT(0)
#define GPDMA_CSR_TCF    PBL_BIT(8)
#define GPDMA_CSR_HTF    PBL_BIT(9)
#define GPDMA_CSR_DTEF   PBL_BIT(10)
#define GPDMA_CSR_ULEF   PBL_BIT(11)
#define GPDMA_CSR_USEF   PBL_BIT(12)
#define GPDMA_CSR_SUSPF  PBL_BIT(13)
#define GPDMA_CSR_TOF    PBL_BIT(14)
#define GPDMA_CSR_ALL    PBL_GENMASK(14, 8)
#define GPDMA_CSR_ERRORS (GPDMA_CSR_DTEF | GPDMA_CSR_ULEF | GPDMA_CSR_USEF)

#define GPDMA_CCR_EN        PBL_BIT(0)
#define GPDMA_CCR_RESET     PBL_BIT(1)
#define GPDMA_CCR_SUSP      PBL_BIT(2)
#define GPDMA_CCR_TCIE      PBL_BIT(8)
#define GPDMA_CCR_HTIE      PBL_BIT(9)
#define GPDMA_CCR_DTEIE     PBL_BIT(10)
#define GPDMA_CCR_ULEIE     PBL_BIT(11)
#define GPDMA_CCR_USEIE     PBL_BIT(12)
#define GPDMA_CCR_LSM       PBL_BIT(16)
#define GPDMA_CCR_LAP       PBL_BIT(17)
#define GPDMA_CCR_PRIO_MASK PBL_GENMASK(23, 22)

#define GPDMA_CTR1_SDW_MASK PBL_GENMASK(1, 0)
#define GPDMA_CTR1_SINC     PBL_BIT(3)
#define GPDMA_CTR1_SAP      PBL_BIT(14)
#define GPDMA_CTR1_DDW_MASK PBL_GENMASK(17, 16)
#define GPDMA_CTR1_DINC     PBL_BIT(19)
#define GPDMA_CTR1_DAP      PBL_BIT(30)

#define GPDMA_CTR2_REQSEL_MASK PBL_GENMASK(6, 0)
#define GPDMA_CTR2_DREQ        PBL_BIT(10)
#define GPDMA_CTR2_TCEM_MASK   PBL_GENMASK(31, 30)

#define GPDMA_CBR1_BNDT_MASK PBL_GENMASK(15, 0)

#define GPDMA_CLBAR_LBA_MASK PBL_GENMASK(31, 16)

/* The linked-list item to load next, and which registers it holds, in their order */
#define GPDMA_CLLR_LA_MASK PBL_GENMASK(15, 2)
#define GPDMA_CLLR_ULL     PBL_BIT(16)
#define GPDMA_CLLR_UDA     PBL_BIT(27)
#define GPDMA_CLLR_USA     PBL_BIT(28)
#define GPDMA_CLLR_UB1     PBL_BIT(29)

/* Requests of the peripherals the drivers use */
#define STM32U5_GPDMA_REQ_LPUART1_RX 34U
#define STM32U5_GPDMA_REQ_LPUART1_TX 35U
