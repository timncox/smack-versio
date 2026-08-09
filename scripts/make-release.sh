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
# create a GitHub release, or upload anything: this firmware has never run on
# hardware, and publishing it is a decision for a human who has heard it. When
# that changes, the zip is ready to attach.
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

echo "==> firmware"
make -C firmware

if [ ! -f "$BIN" ]; then
    echo "error: $BIN was not produced" >&2
    exit 1
fi

echo "==> faceplate"
python3 faceplate/make_faceplate.py >/dev/null

rm -rf "$STAGE"
mkdir -p "$STAGE/faceplate"

cp "$BIN"                                   "$STAGE/smack_versio.bin"
cp MANUAL.md DISCLAIMER.md                  "$STAGE/"
cp firmware/FLASHING.md                     "$STAGE/"
cp faceplate/smack-versio-printsheet.svg    "$STAGE/faceplate/"
cp faceplate/smack-versio-faceplate.svg     "$STAGE/faceplate/"
cp faceplate/MEASURE.md                     "$STAGE/faceplate/"

# The bin is the only file whose integrity matters after transit.
( cd "$STAGE" && shasum -a 256 smack_versio.bin > SHA256SUMS )

cat > "$STAGE/README.txt" <<EOF
Smack Versio $VERSION
Alternative firmware for the Noise Engineering Versio.

READ DISCLAIMER.md FIRST. This is third-party firmware, it is not a Noise
Engineering product, and NE cannot support it.

THIS VERSION HAS NEVER BEEN RUN ON HARDWARE. It compiles and everything
testable without a module passes, but nobody has heard it. Flashing it makes
you its first hardware test.

  smack_versio.bin   the firmware
  FLASHING.md        how to install it, and how to go back to stock
  MANUAL.md          what the controls do
  DISCLAIMER.md      what you are agreeing to
  faceplate/         printable panel overlay -- print at 100%, check the ruler
  SHA256SUMS         checksum for the binary

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
echo "Not published. Tagging and releasing is a human decision -- this build"
echo "has never run on a module."
