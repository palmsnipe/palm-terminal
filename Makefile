SHELL := /bin/bash

ROOT := $(CURDIR)
PALM_TOOLCHAIN_ROOT ?= $(ROOT)/../palm-toolchain
TOOLCHAIN_ROOT := $(abspath $(PALM_TOOLCHAIN_ROOT))
PREFIX := $(if $(PALM_TOOLCHAIN_PREFIX),$(PALM_TOOLCHAIN_PREFIX),$(TOOLCHAIN_ROOT)/.toolchain/prefix)
SDK := $(PREFIX)/palmdev/sdk/include
CC := $(PREFIX)/bin/m68k-none-elf-gcc
OBJCOPY := $(PREFIX)/bin/m68k-none-elf-objcopy
READELF := $(PREFIX)/bin/m68k-none-elf-readelf
SIZE := $(PREFIX)/bin/m68k-none-elf-size
PILRC := $(PREFIX)/bin/pilrc
BUILD_PRC := $(PREFIX)/bin/build-prc

# Preserve the database name so the PRC replaces earlier versions in place.
APP_DB_NAME := Palm Terminal
APP_ID := Terminal
APP_CREATOR := PlTm
APP_TYPE := appl
BUILD_DIR := $(ROOT)/build
ELF := $(BUILD_DIR)/code0001.68k.elf
CODE := $(BUILD_DIR)/code0001.bin
RESOURCES := $(BUILD_DIR)/$(APP_ID).ro
PRC := $(BUILD_DIR)/$(APP_ID).prc
OBJECTS := $(addprefix $(BUILD_DIR)/,main.o bridge.o storage.o network.o settings.o stylus.o startup.o)
DEPS := $(OBJECTS:.o=.d)

SDK_INCLUDES := -isystem$(SDK) -isystem$(SDK)/Core \
	-isystem$(SDK)/Core/Hardware -isystem$(SDK)/Core/System \
	-isystem$(SDK)/Core/UI -isystem$(SDK)/Dynamic \
	-isystem$(SDK)/Libraries -isystem$(SDK)/Extensions/ExpansionMgr
ABI_FLAGS := -Wno-multichar -m68000 -mno-align-int -mno-pcrel -fno-pic \
	-fshort-enums -mshort
CFLAGS := $(ABI_FLAGS) -std=gnu17 -Os -Wall -Wextra -Wno-unused-parameter \
	-Wno-old-style-declaration -MMD -MP -ffunction-sections -fdata-sections \
	-I$(ROOT)/resources -I$(BUILD_DIR) $(SDK_INCLUDES) $(if $(PALM_PROFILE),-DPT_PROFILE)
LDFLAGS := $(ABI_FLAGS) -Os -nostartfiles -Wl,--gc-sections \
	-Wl,-T,$(ROOT)/linker.lkr -Wl,-Map,$(BUILD_DIR)/$(APP_ID).map

.PHONY: all test clean

all: $(PRC)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c resources/resource.h Makefile | $(BUILD_DIR)
	@test -x "$(CC)" || { echo "Missing Palm compiler; run 'make bootstrap' from $(TOOLCHAIN_ROOT)" >&2; exit 1; }
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJECTS) linker.lkr
	$(CC) -o $@.stage1 $(LDFLAGS) -Wl,--emit-relocs $(OBJECTS)
	node $(TOOLCHAIN_ROOT)/tools/generate-m68k-relocs.mjs \
		$@.stage1 $(BUILD_DIR)/reloc_offsets.s $(READELF)
	$(CC) $(ABI_FLAGS) -c $(BUILD_DIR)/reloc_offsets.s -o $(BUILD_DIR)/reloc_offsets.o
	$(CC) -o $@ $(LDFLAGS) -Wl,--emit-relocs $(OBJECTS) $(BUILD_DIR)/reloc_offsets.o
	rm -f $@.stage1

$(CODE): $(ELF)
	$(OBJCOPY) -O binary $< $@ -j .vec -j .text

$(RESOURCES): $(CODE) resources/app.rcp resources/resource.h $(BUILD_DIR)/arm-resources.rcp $(BUILD_DIR)/raw-pen.bin tools/icon.py
	python3 tools/icon.py $(BUILD_DIR)/icons
	cd $(BUILD_DIR) && $(PILRC) -ro -o $@ -creator $(APP_CREATOR) \
		-type $(APP_TYPE) -name '$(APP_DB_NAME)' -I $(ROOT)/resources -I $(BUILD_DIR) \
		$(ROOT)/resources/app.rcp

$(PRC): $(RESOURCES)
	$(BUILD_PRC) --no-check -o $@ -n '$(APP_DB_NAME)' -c $(APP_CREATOR) \
		-t $(APP_TYPE) $(RESOURCES)
	$(SIZE) $(ELF)
	@echo "Built $@"

test: $(PRC) host-test
	node $(TOOLCHAIN_ROOT)/tools/validate-prc.mjs \
		$(PRC) '$(APP_DB_NAME)' $(APP_TYPE) $(APP_CREATOR)
	python3 tests/relocations.py $(READELF) $(ELF) $(CODE)
	python3 tools/arm_image.py verify $(PRC) $(BUILD_DIR)/arm.bin

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)

ARM_CC ?= clang
ARM_TOOLS := $(PREFIX)/bin/arm-none-eabi
ARM_LIBGCC := $(shell $(ARM_TOOLS)-gcc -march=armv4t -print-libgcc-file-name)

$(BUILD_DIR)/raw-pen.o: src/raw-pen.S | $(BUILD_DIR)
	$(ARM_TOOLS)-as -march=armv4t $< -o $@

$(BUILD_DIR)/raw-pen.bin: $(BUILD_DIR)/raw-pen.o
	$(ARM_TOOLS)-objcopy -O binary -j .text $< $@
DEP_ROOT := $(BUILD_DIR)/deps
WOLFSSL := $(DEP_ROOT)/wolfssl
WOLFSSH := $(DEP_ROOT)/wolfssh
VTERM := $(DEP_ROOT)/vterm
OPENSSH := $(DEP_ROOT)/openssh
WOLF_CRYPTO_SOURCES := aes asn coding curve25519 ed25519 ecc error fe_operations ge_operations hmac kdf memory random sha256 sha512 sha3 wc_mlkem wc_mlkem_poly wolfmath sp_int sp_c32 hash signature wc_port
SSH_SOURCES := ssh io port ossh
VTERM_SOURCES := encoding keyboard mouse parser pen screen state unicode vterm
NATIVE_SOURCES := src/backend.c src/key.c src/entropy.c $(BUILD_DIR)/ssh-internal.c \
	$(addsuffix .c,$(addprefix $(WOLFSSL)/wolfcrypt/src/,$(WOLF_CRYPTO_SOURCES))) \
	$(addsuffix .c,$(addprefix $(WOLFSSH)/src/,$(SSH_SOURCES))) \
	$(addsuffix .c,$(addprefix $(VTERM)/src/,$(VTERM_SOURCES))) \
	$(BUILD_DIR)/bcrypt-pbkdf.c $(OPENSSH)/openbsd-compat/blowfish.c
ARM_SOURCES := src/arm_entry.S src/arm_runtime.c src/blowfish_arm.S $(NATIVE_SOURCES)
NATIVE_INCLUDES := -Isrc -I$(WOLFSSL) -I$(WOLFSSH) -I$(VTERM)/include -Isrc/bcrypt -I$(OPENSSH)/openbsd-compat
NATIVE_FLAGS := -std=gnu17 -O2 -DWOLFSSL_USER_SETTINGS -DNDEBUG $(NATIVE_INCLUDES)
ARM_FLAGS := --target=armv4t-none-eabi -march=armv4t -marm -ffreestanding -fno-builtin \
	-fno-pic -fno-jump-tables -ffunction-sections -fdata-sections -fno-unwind-tables \
	-fno-stack-protector -Isrc/freestanding -I$(BUILD_DIR) -isystem $(PREFIX)/m68k-none-elf/include -DPT_ARMLET $(if $(PALM_PROFILE),-DPT_PROFILE)

$(DEP_ROOT)/.ready: tools/dependencies.py
	python3 $< $(DEP_ROOT)
	touch $@

$(BUILD_DIR)/ssh-internal.c: tools/ports.py $(DEP_ROOT)/.ready | $(BUILD_DIR)
	python3 $< ssh $(DEP_ROOT) $@

$(BUILD_DIR)/bcrypt-pbkdf.c: tools/ports.py $(DEP_ROOT)/.ready | $(BUILD_DIR)
	python3 $< bcrypt $(DEP_ROOT) $@

$(BUILD_DIR)/terminal-font.h: tools/font.py $(DEP_ROOT)/.ready | $(BUILD_DIR)
	python3 $< $(DEP_ROOT)/font $@

$(BUILD_DIR)/arm.bin: $(BUILD_DIR)/ssh-internal.c $(BUILD_DIR)/bcrypt-pbkdf.c $(BUILD_DIR)/terminal-font.h $(wildcard src/*.h src/freestanding/*.h src/bcrypt/*.h) \
		src/backend.c src/key.c src/entropy.c src/arm_runtime.c src/arm_entry.S src/blowfish_arm.S armlet.lkr Makefile
	mkdir -p $(BUILD_DIR)/arm
	set -e; i=0; for source in $(ARM_SOURCES); do \
		$(ARM_CC) $(ARM_FLAGS) $(NATIVE_FLAGS) -c "$$source" -o $(BUILD_DIR)/arm/$$i.o; \
		i=$$((i+1)); done; \
		$(ARM_TOOLS)-ld --gc-sections --emit-relocs -Map $(BUILD_DIR)/arm/arm.map \
			-T armlet.lkr -o $(BUILD_DIR)/arm/arm.elf $$(seq 0 $$((i-1)) | sed "s|^|$(BUILD_DIR)/arm/|; s|$$|.o|") $(ARM_LIBGCC)
	$(ARM_TOOLS)-objcopy -O binary -j .image $(BUILD_DIR)/arm/arm.elf $@

$(BUILD_DIR)/arm-resources.rcp: $(BUILD_DIR)/arm.bin tools/arm_image.py
	python3 tools/arm_image.py pack $(ARM_TOOLS)-readelf $(BUILD_DIR)/arm/arm.elf $< $(BUILD_DIR)

HOST_CC ?= clang
.PHONY: host-test
host-test: $(BUILD_DIR)/host-test
	$(BUILD_DIR)/host-test stylus
	python3 tests/interop.py $(BUILD_DIR)/host-test

$(BUILD_DIR)/host-test: $(BUILD_DIR)/ssh-internal.c $(BUILD_DIR)/bcrypt-pbkdf.c $(wildcard src/*.h src/bcrypt/*.h) \
		src/backend.c src/key.c src/entropy.c tests/host.c Makefile
	$(HOST_CC) $(NATIVE_FLAGS) tests/host.c $(NATIVE_SOURCES) -o $@
