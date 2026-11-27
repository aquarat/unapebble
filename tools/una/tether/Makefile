# SPDX-FileCopyrightText: 2026 The unapebble authors
# SPDX-License-Identifier: Apache-2.0
#
# The tether stub, its example experiments and the host simulator the tests
# run. In the build container, from the repository root:
#
#   docker/build.sh -- make -C tools/una/tether          # stub .ota and experiments
#   docker/build.sh -- make -C tools/una/tether test     # host tool tests
#
# arm-none-eabi-gcc must be on PATH, or CROSS set to its prefix.

SDK_BIN ?= $(wildcard /opt/pebbleos-sdk-0.1.17/arm-none-eabi/bin/)
CROSS ?= $(if $(SDK_BIN),$(SDK_BIN)arm-none-eabi-,arm-none-eabi-)
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size
HOSTCC ?= cc
PYTHON ?= python3

REPO := $(abspath $(CURDIR)/../../..)
OUT ?= build
VERSION ?= $(shell git describe --tags --match 'v[0-9]*' --always --dirty 2>/dev/null || echo dev)

ARCH := -mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
WARN := -Wall -Wextra -Wno-unused-parameter
CFLAGS := $(ARCH) -Os -g3 -std=gnu17 -ffunction-sections -fdata-sections -ffreestanding \
	$(WARN) -I$(REPO)/soc/st/include -I$(REPO)/include -Istub
LDLIBS := --specs=nano.specs --specs=nosys.specs -lc -lgcc

STUB_SRCS := stub/startup.S stub/main.c stub/clock.c stub/gpio.c stub/usb.c stub/display.c \
	stub/i2c.c stub/flash.c stub/fmt.c stub/port.c stub/proto.c
STUB_HDRS := $(wildcard stub/*.h)

EXPERIMENTS := rcc_clocks max17262 emmc_cid emmc_read emmc_read_ram emmc_write_test emmc_write_ram
EXP_COMMON := experiments/blob_crt0.c
EXP_HDRS := experiments/tether_blob.h stub/tether_api.h stub/tether_proto.h

.PHONY: all stub experiments sim test clean
# The ELFs stay, for addr2line and gdb
.SECONDARY:

all: stub experiments

stub: $(OUT)/tether.ota

$(OUT)/tether.elf: $(STUB_SRCS) $(STUB_HDRS) stub/stub.ld Makefile
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DTETHER_VERSION='"$(VERSION)"' -nostartfiles -Tstub/stub.ld \
		-Wl,--gc-sections -Wl,-Map=$(OUT)/tether.map -o $@ $(STUB_SRCS) $(LDLIBS)
	$(SIZE) $@

$(OUT)/tether.bin: $(OUT)/tether.elf
	$(OBJCOPY) -O binary $< $@

$(OUT)/tether.ota: $(OUT)/tether.bin
	$(PYTHON) $(REPO)/tools/una/pack_ota.py $< -v "$(VERSION)" -o $@

experiments: $(EXPERIMENTS:%=$(OUT)/experiments/%.bin)

$(OUT)/experiments/%.elf: experiments/%.c $(EXP_COMMON) $(EXP_HDRS) experiments/blob.ld Makefile
	@mkdir -p $(OUT)/experiments
	$(CC) $(CFLAGS) -Iexperiments -DBLOB_NAME='"$*"' -nostartfiles -Texperiments/blob.ld \
		-Wl,--gc-sections -Wl,--no-warn-rwx-segments -o $@ $(EXP_COMMON) $< $(LDLIBS)

$(OUT)/experiments/%.bin: $(OUT)/experiments/%.elf
	$(OBJCOPY) -O binary $< $@

# The protocol as the stub runs it, on the host, for the tests
SIM_SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all
sim: $(OUT)/tether_sim

$(OUT)/tether_sim: sim/sim_port.c stub/proto.c stub/port.h stub/tether_proto.h Makefile
	@mkdir -p $(OUT)
	$(HOSTCC) -O1 -g -std=gnu17 -Wall -Wextra -Wno-unused-parameter $(SIM_SANITIZE) -Istub -o $@ \
		sim/sim_port.c stub/proto.c

test: $(OUT)/tether_sim
	TETHER_SIM=$(abspath $(OUT)/tether_sim) $(PYTHON) -m unittest -v test_tether

clean:
	rm -rf $(OUT)
