# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_battery_policy
  INCLUDES
    pebble/services/battery/gauge
)

pbl_clar_test(test_battery_test
  INCLUDES
    pebble/services/battery/test
  SOURCES
    pebble/services/battery/test/core.c
)
