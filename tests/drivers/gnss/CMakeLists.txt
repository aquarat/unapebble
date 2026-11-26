# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_gnss
  SOURCES
    drivers/base/device.c
    drivers/gnss/gnss.c
    drivers/gnss/sim_track.c
    pebble/services/location/service.c
  INCLUDES
    drivers/gnss
  DEFINES
    CONFIG_DEVICE_LOG_LEVEL=0
    CONFIG_DRIVER_GNSS_LOG_LEVEL=0
    CONFIG_SERVICE_LOCATION_LOG_LEVEL=0
)
