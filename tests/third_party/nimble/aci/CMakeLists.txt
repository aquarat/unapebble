# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_aci_gatts_map
  SOURCES
    third_party/nimble/aci/gatts_map.c
  INCLUDES
    third_party/nimble/aci
    third_party/nimble/port/include
    third_party/nimble/mynewt-nimble/nimble/include
    third_party/nimble/mynewt-nimble/nimble/host/include
    third_party/nimble/mynewt-nimble/porting/nimble/include
)
