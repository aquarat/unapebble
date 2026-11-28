/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/util/bits.h>

/* Register offsets of the SPI with TSIZE and a FIFO, from the STM32H7 on, as on the STM32U5 */

#define STM32_SPI_CR1_OFFSET  0x0000
#define STM32_SPI_CR2_OFFSET  0x0004
#define STM32_SPI_CFG1_OFFSET 0x0008
#define STM32_SPI_CFG2_OFFSET 0x000c
#define STM32_SPI_IER_OFFSET  0x0010
#define STM32_SPI_SR_OFFSET   0x0014
#define STM32_SPI_IFCR_OFFSET 0x0018
#define STM32_SPI_TXDR_OFFSET 0x0020
#define STM32_SPI_RXDR_OFFSET 0x0030

/* Register bit definitions */

#define SPI_CR1_SPE    PBL_BIT(0)
#define SPI_CR1_CSTART PBL_BIT(9)
#define SPI_CR1_SSI    PBL_BIT(12)

#define SPI_CR2_TSIZE_MASK PBL_GENMASK(15, 0)

#define SPI_CFG1_DSIZE_MASK PBL_GENMASK(4, 0)
#define SPI_CFG1_FTHLV_MASK PBL_GENMASK(8, 5)
#define SPI_CFG1_MBR_MASK   PBL_GENMASK(30, 28)
#define SPI_CFG1_BPASS      PBL_BIT(31)

#define SPI_CFG2_MASTER  PBL_BIT(22)
#define SPI_CFG2_LSBFRST PBL_BIT(23)
#define SPI_CFG2_CPHA    PBL_BIT(24)
#define SPI_CFG2_CPOL    PBL_BIT(25)
#define SPI_CFG2_SSM     PBL_BIT(26)
#define SPI_CFG2_AFCNTR  PBL_BIT(31)

#define SPI_IER_RXPIE  PBL_BIT(0)
#define SPI_IER_TXPIE  PBL_BIT(1)
#define SPI_IER_EOTIE  PBL_BIT(3)
#define SPI_IER_OVRIE  PBL_BIT(6)
#define SPI_IER_MODFIE PBL_BIT(9)

/* SR flags; those that are cleared are cleared by the same bits in IFCR */
#define SPI_SR_RXP  PBL_BIT(0)
#define SPI_SR_TXP  PBL_BIT(1)
#define SPI_SR_EOT  PBL_BIT(3)
#define SPI_SR_TXTF PBL_BIT(4)
#define SPI_SR_UDR  PBL_BIT(5)
#define SPI_SR_OVR  PBL_BIT(6)
#define SPI_SR_MODF PBL_BIT(9)

#define SPI_IFCR_ALL                                                                            \
  (SPI_SR_EOT | SPI_SR_TXTF | SPI_SR_UDR | SPI_SR_OVR | PBL_BIT(7) | PBL_BIT(8) | SPI_SR_MODF | \
   PBL_BIT(11))

/* The largest transfer TSIZE counts */
#define SPI_TSIZE_MAX 0xffffU
