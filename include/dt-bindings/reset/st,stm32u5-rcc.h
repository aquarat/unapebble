/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef _DT_BINDINGS_RESET_ST_STM32U5_RCC_H
#define _DT_BINDINGS_RESET_ST_STM32U5_RCC_H

/* STM32U5 RCC resets: the bit of a peripheral in its reset register, eight per byte of the
   offset of the register in the RCC */

#define STM32U5_AHB1_RESET(bit)  ((bit) + (0x60 * 8))
#define STM32U5_AHB2_RESET(bit)  ((bit) + (0x64 * 8))
#define STM32U5_AHB3_RESET(bit)  ((bit) + (0x6c * 8))
#define STM32U5_APB1L_RESET(bit) ((bit) + (0x74 * 8))
#define STM32U5_APB1H_RESET(bit) ((bit) + (0x78 * 8))
#define STM32U5_APB2_RESET(bit)  ((bit) + (0x7c * 8))
#define STM32U5_APB3_RESET(bit)  ((bit) + (0x80 * 8))

#endif /* _DT_BINDINGS_RESET_ST_STM32U5_RCC_H */
