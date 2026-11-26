# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SHELL_SDK)
  set(source)
  file(GLOB _glob0 CONFIGURE_DEPENDS timeline/*.c)
  list(APPEND source ${_glob0})
  file(GLOB_RECURSE _glob1 CONFIGURE_DEPENDS launcher/default/*.c)
  list(APPEND source ${_glob1})
  list(APPEND source app_fetch_ui.c watchfaces.c)
else()
  file(GLOB_RECURSE source CONFIGURE_DEPENDS *.c)
  if(NOT CONFIG_APP_SPORT)
    list(FILTER source EXCLUDE REGEX "^${CMAKE_CURRENT_SOURCE_DIR}/sport/")
  endif()
endif()
pbl_library()
pbl_library_sources(${source})
