/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/**
 * @addtogroup soc_st
 * @{
 */

/**
 * @brief Invalidate the STM32U5 ICACHE, after the flash it caches was programmed or erased.
 *
 * The ICACHE sits on the code bus, which carries data reads of the flash as well as instruction
 * fetches: reads of a range just changed may otherwise return what it held before. A no-op when
 * the cache is off.
 */
void soc_stm32u5_icache_invalidate(void);

/** @} */
