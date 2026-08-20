#!/usr/bin/env bash
#
# Assemble a distributable Smack Versio release, locally.
#
#   ./scripts/make-release.sh
#
# Produces dist/smack-versio-<version>.zip containing the firmware binary, the
# manual, the disclaimer, the flashing guide and the faceplate template -- the
# same shape WTF! and FRGMNTS ship (DESIGN.md section 10).
#
# This script deliberately stops at a file on disk. It does not tag, push,
# create a GitHub release, or upload anything -- publishing firmware is a
# decision for a human. The zip is ready to attach when that decision is made.
#
# Note on release.json: the Schwung store pattern does NOT apply here. That
# release.json is read by schwung-manager to install Move module *tarballs*;
# a Versio .bin has no such installer, so shipping one would advertise an
# install path that does not exist.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"
VERSION="$(tr -d ' \n' < VERSION)"
STAGE="dist/smack-versio-$VERSION"
BIN="firmware/build/smack_versio.bin"

echo "==> Smack Versio $VERSION"

echo "==> native tests"
make -f firmware/Makefile.test

# Two builds ship. BOOT_NONE is the headline one: it installs from Noise
# Engineering's own firmware page, which is the path almost everyone will want.
# BOOT_SRAM stays for anyone who would rather use the Daisy tooling, and
# because it has room to grow that BOOT_NONE does not.
echo "==> firmware: BOOT_NONE (internal flash, NE-uploader compatible)"
make -C firmware clean >/dev/null
make -C firmware APP_TYPE=BOOT_NONE USE_LTO=1

if [ ! -f "$BIN" ]; then
    echo "error: $BIN was not produced" >&2
    exit 1
fi

# A BOOT_NONE image that overflows internal flash fails at link, so reaching
# here means it fits. Guard the other direction: if libDaisy has not been
# rebuilt at -Os -flto the link would have failed, but a stale build/ could
# leave a BOOT_SRAM binary sitting here under the wrong name. 128 KB is the
# hard ceiling; anything larger is not the build we think it is.
BOOT_NONE_BYTES=$(wc -c < "$BIN" | tr -d ' ')
if [ "$BOOT_NONE_BYTES" -gt 131072 ]; then
    echo "error: BOOT_NONE image is $BOOT_NONE_BYTES bytes, over the 131072 limit." >&2
    echo "       libDaisy probably needs rebuilding at -Os -flto; see firmware/BOOT_NONE.md" >&2
    exit 1
fi
cp "$BIN" "$ROOT/dist/.boot_none.bin"

echo "==> firmware: BOOT_SRAM (QSPI, needs the Daisy bootloader)"
make -C firmware clean >/dev/null
make -C firmware
cp "$BIN" "$ROOT/dist/.boot_sram.bin"

echo "==> faceplate"
python3 faceplate/make_faceplate.py >/dev/null

rm -rf "$STAGE"
mkdir -p "$STAGE/faceplate"

cp "$ROOT/dist/.boot_none.bin"              "$STAGE/smack_versio.bin"
cp "$ROOT/dist/.boot_sram.bin"              "$STAGE/smack_versio-bootsram.bin"
rm -f "$ROOT/dist/.boot_none.bin" "$ROOT/dist/.boot_sram.bin"
cp MANUAL.md DISCLAIMER.md                  "$STAGE/"
cp firmware/FLASHING.md                     "$STAGE/"
cp faceplate/smack-versio-printsheet.svg    "$STAGE/faceplate/"
cp faceplate/smack-versio-faceplate.svg     "$STAGE/faceplate/"
cp faceplate/MEASURE.md                     "$STAGE/faceplate/"

# The bin is the only file whose integrity matters after transit.
( cd "$STAGE" && shasum -a 256 smack_versio.bin smack_versio-bootsram.bin > SHA256SUMS )

cat > "$STAGE/README.txt" <<EOF
Smack Versio $VERSION
Alternative firmware for the Noise Engineering Versio.

READ DISCLAIMER.md FIRST. This is third-party firmware, it is not a Noise
Engineering product, and NE cannot support it.

THIS BUILD STILL NEEDS TESTING. The firmware has been played on a module --
audio, capture, the knob layout and the effects are all confirmed by ear -- but
this exact binary carries two fixes that have not yet been run on hardware:
LIVE mode (which had never fired at all before now) and the loop recorder's
buffer sizing. Both are covered by native tests. Neither has been heard.

The boot CPU report has also never been read, so the headroom figure the whole
design rests on is still unconfirmed. Play hard, power cycle, read the LED bar
-- that is the one measurement worth sending back.

  smack_versio.bin              the firmware -- flash this one
  smack_versio-bootsram.bin     alternative build, see FLASHING.md
  FLASHING.md                   how to install it, and how to go back to stock
  MANUAL.md          what the controls do
  DISCLAIMER.md      what you are agreeing to
  faceplate/         printable panel overlay -- print at 100%, check the ruler
  SHA256SUMS         checksum for the binary

TO INSTALL: put the module in DFU (hold BOOT, tap RESET, release BOOT) and
give smack_versio.bin to Noise Engineering's firmware page. That is the whole
procedure -- no terminal, no bootloader, no extra tools. Confirmed working
2026-08-20.

  https://portal.noiseengineering.us/

smack_versio-bootsram.bin is the same firmware built to run from QSPI via the
Daisy bootloader instead. It has more room to grow but needs the bootloader
installed once and a tool to flash it. FLASHING.md covers that route, and it
drives \`make\` targets from the source repo -- flash it from a clone.

Installing is reversible: flashing any official Noise Engineering firmware
restores the module to stock.
EOF

( cd dist && rm -f "smack-versio-$VERSION.zip" \
  && zip -qr "smack-versio-$VERSION.zip" "smack-versio-$VERSION" )

echo
echo "==> dist/smack-versio-$VERSION.zip"
ls -l "$ROOT/dist/smack-versio-$VERSION.zip"
# BSD head has no -n -N, so filter rather than trim from the end.
unzip -l "$ROOT/dist/smack-versio-$VERSION.zip" | awk 'NR>3 && NF>3 && $1 != "----"'
echo
echo "Not published. Tagging and releasing is a human decision."
