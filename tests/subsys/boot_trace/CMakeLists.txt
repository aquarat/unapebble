# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_boot_trace
  SOURCES
    subsys/boot_trace/boot_trace.c
  DEFINES
    CONFIG_SOC_STM32U5=1
    CONFIG_BOOT_TRACE=1
    CONFIG_BOOT_TRACE_RAM_BASE=test_boot_trace_ram
    CONFIG_BOOT_TRACE_RAM_SIZE=0x400
    CONFIG_BOOT_TRACE_BKP_CRUMB=30
    CONFIG_BOOT_TRACE_BKP_BOOT=31
    CONFIG_BOOT_TRACE_IMAGE="test"
    CONFIG_BOOT_TRACE_IMAGE_ID=1
)
