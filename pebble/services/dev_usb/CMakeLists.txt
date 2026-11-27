# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_library()
pbl_library_sources(dev_usb.c)
if(CONFIG_DEV_USB_TETHER)
  # The stub's own protocol code, so that tether.py speaks to both alike
  pbl_library_sources(tether.c ${PBL_BASE}/tools/una/tether/stub/proto.c)
  pbl_library_include_directories(${PBL_BASE}/tools/una/tether/stub)
endif()
