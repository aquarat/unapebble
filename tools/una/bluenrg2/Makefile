# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
#
# The code patch.py adds to the BlueNRG-2's DTM v3.2.0. Build in the container:
#   docker/build.sh -- make -C tools/una/bluenrg2

CROSS ?= arm-none-eabi-
OUT := build
BASE := 0x10068000
# The return of the DTM's HCI parser (its pop {r4, pc}), Thumb
PARSER_RETURN := 0x1004e491

CFLAGS := -mcpu=cortex-m0 -mthumb -Os -ffreestanding -fno-builtin -nostdlib -Wall -Wextra \
	-Werror -ffunction-sections
ASFLAGS := -mcpu=cortex-m0 -mthumb -DPARSER_RETURN=$(PARSER_RETURN)

all: $(OUT)/acl_patch.bin

$(OUT)/acl_patch.elf: acl_patch.c acl_patch_entry.S acl_patch.ld
	mkdir -p $(OUT)
	$(CROSS)gcc $(CFLAGS) -x assembler-with-cpp $(ASFLAGS) -c acl_patch_entry.S -o $(OUT)/entry.o
	$(CROSS)gcc $(CFLAGS) -c acl_patch.c -o $(OUT)/acl_patch.o
	$(CROSS)ld -T acl_patch.ld --defsym=ACL_PATCH_BASE=$(BASE) $(OUT)/entry.o \
		$(OUT)/acl_patch.o -o $@ -Map $(OUT)/acl_patch.map
	$(CROSS)nm $@ > $(OUT)/acl_patch.sym

$(OUT)/acl_patch.bin: $(OUT)/acl_patch.elf
	$(CROSS)objcopy -O binary $< $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
