# Smack Versio — ARM firmware build.
#
#   make -C firmware              build smack_versio.bin
#   make -C firmware program-dfu  flash over USB (module out of the case)
#
# Requires libDaisy checked out and built:
#   git clone --recurse-submodules https://github.com/electro-smith/libDaisy \
#       ~/tim-os/daisy-sdk/libDaisy && make -C ~/tim-os/daisy-sdk/libDaisy
#
# DaisySP is deliberately NOT used — smack_core is self-contained DSP.

TARGET = smack_versio

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# BOOT_SRAM, not the BOOT_NONE default, because the STM32H750 has only 128 KB
# of INTERNAL flash and this app is ~137 KB. Measured breakdown: the engine is
# 22.5 KB and this shim ~3.5 KB -- the other ~112 KB is libDaisy and the ST
# HAL, which DaisySeed::Init() pulls in wholesale (including both USB stacks).
#
# Tried and rejected for BOOT_NONE: dropping the USB serial logger (-7.1 KB,
# still 7.6 KB over) and LTO (-1.7 KB; can't do better because libDaisy.a
# carries no LTO bytecode, so set USE_LTO=1 only if you rebuild libDaisy with
# -flto too). Shaving the last ~6 KB would leave a build that breaks the next
# time anyone touches the engine.
#
# Cost: the module needs the Daisy bootloader in internal flash (see
# FLASHING.md). This is reversible -- flashing any official Noise Engineering
# firmware writes internal flash and removes the bootloader again.
APP_TYPE ?= BOOT_SRAM

CPP_SOURCES = smack_versio.cpp

# smack_core_versio.c #includes vendor/smack_core.c with calloc/free
# redirected to SDRAM. Never list vendor/smack_core.c here as well —
# that would define every engine symbol twice.
C_SOURCES = clock_adapter.c versio_alloc.c smack_core_versio.c

# The engine ships lo-fi varispeed on purpose; -ffast-math would also let the
# compiler reorder the float paths the seeded FX depend on. Leave it off.
OPT = -Os

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Link-time optimization. The STM32H750 has only 128 KB of internal flash and
# this app is ~137 KB without it, so LTO is what decides whether a plain
# BOOT_NONE build fits. Applied after the include so it appends to libDaisy's
# own LDFLAGS rather than replacing them.
ifeq ($(USE_LTO), 1)
override CFLAGS   += -flto
override CPPFLAGS += -flto
override LDFLAGS  += -flto -Os
endif

# Diagnostic build: blink LED 3 from the top of the main loop, before any
# engine call, so a dark panel tells you whether the loop is running at all.
# See DIAGNOSE.md. Always clean first -- the sources are identical between a
# diagnostic and a release build, so make will not rebuild on the flag change
# alone and you will happily flash the wrong one.
#
#   make -C firmware clean && make -C firmware DIAG=1 && make -C firmware program-dfu
ifeq ($(DIAG), 1)
override CFLAGS   += -DDIAG_HEARTBEAT
override CPPFLAGS += -DDIAG_HEARTBEAT
endif
