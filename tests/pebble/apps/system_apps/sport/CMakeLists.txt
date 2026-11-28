# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_sport_engine
  SOURCES
    pebble/apps/system/sport/engine.c
    pebble/apps/system/sport/fields.c
)

pbl_clar_test(test_sport_fit
  SOURCES
    pebble/apps/system/sport/fit.c
    pebble/apps/system/sport/journal.c
)
