/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/idle.h>
#include <pbl/soc/st/idle.h>
#include <pbl/util/size.h>

/*
 * Sleep only for now. Stop 2 joins as SOC_STM32_IDLE_STOP, with the stop state of the tree: the
 * system timer runs on LPTIM4 from the LSE, which counts on in Stop 2, but the core leaves Stop 2
 * on MSIS at its wakeup range, with VOS and the EPOD booster down, and the clock tree has to come
 * back first: VOS 1 and the booster, the flash latency, MSIS at its range, PLL1, then the switch
 * of the system clock.
 */
const struct pbl_idle_state pbl_soc_idle_states[] = {
  [SOC_STM32_IDLE_WFI] = {.name = "wfi", .min_ticks = 1, .exit_ticks = 0, .enter = pbl_cpu_idle},
};
const size_t pbl_soc_num_idle_states = PBL_ARRAY_SIZE(pbl_soc_idle_states);
