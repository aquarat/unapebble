# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_blk_nor
  SOURCES
    drivers/flash/blk_nor.c
  INCLUDES
    drivers/flash
)

pbl_clar_test(test_ecc_nor
  SOURCES
    drivers/flash/ecc_nor.c
  INCLUDES
    drivers/flash
)
