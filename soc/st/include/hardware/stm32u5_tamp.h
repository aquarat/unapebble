/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Register offsets: 32 backup registers, kept in the backup domain */

#define STM32U5_TAMP_BKPR_OFFSET(n) (0x0100 + (n) * 4U)
