/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/**
 * Round mask of the LS012B7DD06A panel of the UNA Watch, a 240 pixel circle: the pixels hidden
 * at each end of the rows of its top half. Computed, not from a datasheet, as neither the shape
 * nor the outline of the panel is documented (docs/boards/unawatch/hardware.md, section 5.1).
 */
static const uint8_t pbl_panel_ls012b7dd06a_round_mask[] = {
  109, 101, 95, 91, 87, 84, 81, 78, 75, 73, 70, 68, 66, 64, 62, 61, 59, 57, 55, 54, 52, 51,
  50, 48, 47, 46, 44, 43, 42, 41, 40, 38, 37, 36, 35, 34, 33, 32, 31, 31, 30, 29, 28, 27,
  26, 25, 25, 24, 23, 22, 22, 21, 20, 20, 19, 18, 18, 17, 16, 16, 15, 15, 14, 14, 13, 13,
  12, 12, 11, 11, 10, 10, 9, 9, 8, 8, 8, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4,
  4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
