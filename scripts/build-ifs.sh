#!/bin/sh
# build-ifs.sh — build sensorhub.ifs with mkifs (QNX SDP 8.0).
# Run from the repo root after: source <sdp-install>/qnxsdp-env.sh && make -C build
# Produces: out/<arch>/sensorhub.ifs
# NOTE: the image tool for a hand-written .build file is mkifs
# ($QNX_HOST/usr/bin/mkifs), NOT mkqnximage (host/common/bin/mkqnximage is
# the VM-image generator with different syntax).
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-aarch64}

: "${QNX_HOST:?QNX_HOST is not set — source qnxsdp-env.sh first}"
: "${QNX_TARGET:?QNX_TARGET is not set — source qnxsdp-env.sh first}"

MKIFS="$QNX_HOST/usr/bin/mkifs"
[ -x "$MKIFS" ] || { echo "mkifs not found at $MKIFS"; exit 1; }
[ -f "out/$ARCH/sensorhub" ] || { echo "sensorhub binary missing — run 'make -C build' first"; exit 1; }
[ -f "out/$ARCH/sensorctl" ] || { echo "sensorctl binary missing — run 'make -C build' first"; exit 1; }

# mkifs resolves /bin/sensorhub=sensorhub against the current directory,
# so run it from out/$ARCH where the freshly built binaries live.
cd "out/$ARCH"
"$MKIFS" -v ../../build/sensorhub.build sensorhub.ifs
ls -l sensorhub.ifs
echo "built: out/$ARCH/sensorhub.ifs"
