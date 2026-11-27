# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_bmi270
  SOURCES
    drivers/imu/bmi270/bmi270_config.c
  INCLUDES
    drivers/imu/bmi270
)

pbl_clar_test(test_bmm350
  INCLUDES
    drivers/imu/bmm350
)
