# The early PLL1 power-off as a Kconfig option; it only moves what the clock tree did

- Status: Accepted
- Date: 2026-11-26 23:27 UTC
- Decided by: agent

## Context
`f2775ba34` turned PLL1 off at the start of boot, once on MSIS, for the clock
tree to set PLL1 Q up. But `pbl_clk_stm32u5_init()` already turned PLL1 off
before assigning the clocks (`prv_pll1_off()`, unchanged since A): A started
PLL1 anew too.

## Decision
`CONFIG_SOC_STM32U5_EARLY_PLL1_OFF`, on by default as in B, off in the Lab
as in A.

## Consequences
The difference is only how long PLL1 runs as the bootloader left it, up to
the clock tree's init. The clock change that B really makes is PLL1 Q and
the eMMC on it, which the Lab applies at runtime. Unlikely a loop cause.
