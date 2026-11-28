# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_NIMBLE_ACI_BLUENRG2)
  pbl_library()
  pbl_library_sources(
    gap.c
    gattc.c
    gatts.c
    gatts_map.c
    hs.c
    link.c
    store.c
    svc.c
  )
  pbl_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../transport)
endif()
