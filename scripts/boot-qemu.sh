#!/bin/sh
# boot-qemu.sh — boot sensorhub under QEMU TCG (aarch64 virt, 256 MB).
# Verified working 2026-11-16 (part 6): boots to userspace, sensorhub
# auto-test passes end-to-end, interactive ksh shell on stdio; Ctrl-A X exits.
#
# WHY the shim wrapper (out/aarch64/shim5.elf) and NOT the raw IFS:
# - QEMU's ELF -kernel loader does NOT pass the DTB in x0 (and -dtb is
#   ignored for ELF boots), but startup-qemu-virt needs the FDT for RAM,
#   UART and GIC — without it startup parks in a wfi loop, silent.
# - QEMU reserves a 1 MB DTB region at RAM base 0x40000000-0x40100000 and
#   silently DROPS the DTB if the image overlaps it, so the IFS must sit
#   above 0x40100000 (buildfile [image=0x40200000]).
# - startup overwrites a DTB left at RAM base before the GIC search, so
#   the shim puts the DTB at the TOP of RAM (0x4f000000) and sets x0 to it.
# shim5.elf embeds the IFS via .incbin — rebuild it after any IFS change:
#   as shim5.s -o shim5.o && ld -T shim5.ld shim5.o -o shim5.elf
# (SDP aarch64 as/ld; sources in out/aarch64/shim5.s + shim5.ld).
set -e
cd "$(dirname "$0")/.."
ARCH=${ARCH:-aarch64}
WRAPPER="out/$ARCH/shim5.elf"
[ -f "$WRAPPER" ] || { echo "wrapper not found: $WRAPPER (run scripts/build-ifs.sh first, then rebuild shim5)"; exit 1; }

QEMU=${QEMU:-qemu-system-aarch64}
command -v "$QEMU" >/dev/null 2>&1 || { echo "$QEMU not found — install qemu-system-aarch64 first"; exit 1; }

exec "$QEMU" -machine virt -cpu cortex-a57 -m 256 \
    -kernel "$WRAPPER" -serial stdio -display none
