# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-build rhkd (HomeKitADK + raptor-ipc) and the ADK AccessorySetupGenerator for
# Thingino T41 cameras.
# Dev build: inside the thingino-hap-build image (static musl):  make -C /work/build
# Firmware build: Buildroot package thingino-rhkd passes CROSS, ARCHFLAGS, EXTRA_CFLAGS and STATIC.
CROSS ?= mipsel-linux-musl-
CC := $(CROSS)gcc
STRIP := $(CROSS)strip

SRC := ../src
ADK := $(SRC)/HomeKitADK
MBED := $(SRC)/mbedtls
PALT := $(SRC)/pal-thingino
OUT := out
LOG_LEVEL ?= 3

ARCHFLAGS ?= -march=mips32r2 -mhard-float -EL
STATIC ?= -static
EXTRA_CFLAGS ?=
CFLAGS := $(ARCHFLAGS) $(EXTRA_CFLAGS) -Os -g0 -ffunction-sections -fdata-sections -std=gnu11 \
  -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare \
  -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DIP=1 -DHAP_LOG_LEVEL=$(LOG_LEVEL) -DHAP_ENABLE_DEVELOPMENT_ONLY_CODE=1 \
  -I$(PALT) -I$(ADK)/PAL/POSIX -I$(ADK)/PAL -I$(ADK)/HAP \
  -I$(ADK)/External/HTTP -I$(ADK)/External/JSON -I$(ADK)/External/Base64 \
  -I$(MBED)/include
LDFLAGS := $(ARCHFLAGS) $(STATIC) -Wl,--gc-sections -s -pthread -lm

ADK_SRCS := $(wildcard $(ADK)/HAP/*.c) $(wildcard $(ADK)/PAL/*.c) \
  $(wildcard $(ADK)/External/HTTP/*.c) $(wildcard $(ADK)/External/JSON/*.c) $(wildcard $(ADK)/External/Base64/*.c) \
  $(filter-out %/HAPPlatformServiceDiscovery.c %/HAPPlatformLog.c,$(wildcard $(ADK)/PAL/POSIX/*.c)) \
  $(PALT)/HAPPlatformServiceDiscovery.c $(PALT)/HAPPlatformLog.c \
  $(ADK)/PAL/Crypto/MbedTLS/HAPMbedTLS.c
MBED_SRCS := $(wildcard $(MBED)/library/*.c)
GEN_SRCS := $(ADK)/Tools/AccessorySetupGenerator/Main.c
RHKD_SRCS := $(wildcard $(SRC)/rhkd/*.c)
RIPC_SRCS := $(SRC)/raptor-ipc/src/rss_ring.c $(SRC)/raptor-ipc/src/rss_ipc_log.c $(SRC)/raptor-ipc/src/rss_ctrl.c

obj = $(patsubst $(SRC)/%.c,$(OUT)/obj/%.o,$(1))

all: $(OUT)/hap-setupgen $(OUT)/rhkd

$(OUT)/obj/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	@echo CC $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/libmbedcrypto.a: $(call obj,$(MBED_SRCS))
	$(CROSS)ar crs $@ $^

$(OUT)/libhap.a: $(call obj,$(filter-out %/HAPPlatformSystemInit.c,$(ADK_SRCS)))
	$(CROSS)ar crs $@ $^

$(OUT)/hap-setupgen: $(call obj,$(GEN_SRCS) $(ADK)/PAL/HAPPlatformSystemInit.c) $(OUT)/libhap.a $(OUT)/libmbedcrypto.a
	$(CC) -o $@ -Wl,--start-group $^ -Wl,--end-group $(LDFLAGS)

# raptor-ipc wants GNU extensions; rhkd needs its headers.
$(call obj,$(RIPC_SRCS)): CFLAGS += -D_GNU_SOURCE -I$(SRC)/raptor-ipc/include
$(call obj,$(RHKD_SRCS)): CFLAGS += -I$(SRC)/rhkd -I$(SRC)/raptor-ipc/include

$(OUT)/rhkd: $(call obj,$(RHKD_SRCS) $(RIPC_SRCS) $(ADK)/PAL/HAPPlatformSystemInit.c) $(OUT)/libhap.a $(OUT)/libmbedcrypto.a
	$(CC) -o $@ -Wl,--start-group $^ -Wl,--end-group $(LDFLAGS) -latomic

clean:
	rm -rf $(OUT)
.PHONY: all clean
