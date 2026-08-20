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
#
# -O3, not -Os. The first hardware run reported a stored CPU peak over 90% and
# the audio callback was overrunning its 2.67 ms block, which is what a buzz
# and a starved main loop look like. The engine was being compiled for SIZE --
# every DSP inner loop in smack_core included.
#
# That -Os was inherited from the attempt to squeeze a BOOT_NONE image into
# 128 KB of internal flash. This is a BOOT_SRAM build: the app lives in SRAM,
# where it occupies 151,740 of 480 KB. Size was never the constraint here, so
# -Os was buying nothing and costing throughput on the one axis that can kill
# the project.
#
# If a BOOT_NONE build is ever revived (see BOOT_NONE.md), that configuration
# genuinely is flash-bound and will want -Os back -- set it there, not here.
OPT = -O3

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

# Diagnostic builds. Not for release -- DIAG takes LED 3 away from the status
# display and the CPU alarm.
#
# Always clean first. The sources are identical between a diagnostic and a
# release build, so make will not rebuild on the flag change alone and you will
# flash the wrong image without noticing. Judge by size, not by intent.
#
#   make -C firmware clean && make -C firmware DIAG=3 && make -C firmware program-dfu
#
#   DIAG=1  heartbeat only: blink LED 3 from the top of the main loop, before
#           touching the engine. Blinking means the loop is alive.
#   DIAG=3  boot-stage counter: n green LEDs at each milestone in main(), all
#           blue once the audio ISR is live, then the heartbeat. Whatever it
#           stops on is the step that killed it. Reach for this when a fix
#           fails to move the symptom -- that means the unknown is "how far
#           does it get", not "which line is wrong".
ifeq ($(DIAG), 1)
override CFLAGS   += -DDIAG_HEARTBEAT
override CPPFLAGS += -DDIAG_HEARTBEAT
endif
ifeq ($(DIAG), 3)
override CFLAGS   += -DDIAG_HEARTBEAT -DDIAG_BOOTSTAGE
override CPPFLAGS += -DDIAG_HEARTBEAT -DDIAG_BOOTSTAGE
endif
