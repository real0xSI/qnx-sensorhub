# qnx-sensorhub

A QNX 8.0-native virtual-sensor resource manager + client, built for the
QNX Hackathon 2026 (category: **Low-level Software**).

`sensorhub` is a resource-manager daemon that registers two devices in the
QNX pathname space:

- `/dev/sensor0` — temperature (simulated, slow upward drift)
- `/dev/sensor1` — humidity + pressure (simulated)

`sensorctl` is the client: it reads live values, configures the daemon with
`devctl()` (sampling rate, threshold, stats, reset), and subscribes to a QNX
**pulse** that fires when the temperature crosses a set threshold.

## Why this is genuinely QNX

- Resource manager framework (`resmgr_attach`, `dispatch_create`,
  `iofunc_*`)
- `devctl()` configuration, including a custom subscribe command
  (commands use `__DIOF`/`__DIOT`/`__DION` so the framework knows the
  data direction and size)
- QNX pulses for event notification (server → client channel, delivered
  through a `ConnectAttach(_NTO_SIDE_CHANNEL)` connection id)
- POSIX timer delivering a pulse into the dispatch loop
- Built with `qcc`, packaged into a bootable IFS with `mkifs`, runs under
  QEMU (TCG, no KVM needed)

## Requirements

- QNX SDP 8.0, installed via QNX Software Center (a non-commercial /
  academic license is enough to build and boot this)
- Host `qemu-system-aarch64` (8.x) for the boot test
- A Linux or QNX host with the SDP 8.0 toolchain on `PATH`

## Build

```sh
source <sdp-install>/qnxsdp-env.sh   # sets QNX_HOST, QNX_TARGET
make -C build                        # aarch64 (QEMU virt); or make ARCH=x86_64
# minimal /bin/sleep helper (the SDP target tree ships no sleep/echo):
qcc -Vgcc_ntoaarch64le -O2 -o out/aarch64/sleep src/sleep.c
scripts/build-ifs.sh                 # -> out/aarch64/sensorhub.ifs
```

The IFS is booted through a small shim (`out/aarch64/shim5.elf`) because
QEMU's ELF `-kernel` loader does not pass the device-tree blob in `x0`
(and `-dtb` is ignored for ELF boots), while `startup-qemu-virt` needs
the FDT for RAM, UART and GIC. The shim sets `x0` to the DTB at the top
of RAM and jumps to the IFS entry. **Rebuild the shim after any IFS
change** (from `out/aarch64/`, with the SDP env sourced):

```sh
as shim5.s -o shim5.o && ld -T shim5.ld shim5.o -o shim5.elf
```

## Run (QEMU)

```sh
scripts/boot-qemu.sh                 # 256 MB guest, console on stdio
```

The IFS `.script` runs an auto-test on boot (starts `sensorhub`, reads
both devices, changes the rate to 100 ms, fires the threshold pulse,
prints stats) and then drops to an interactive `ksh` shell. From the
shell you can also drive it by hand:

```sh
sensorhub &
echo /dev/sensor*                    # devices are up (no /bin/ls in the image)
sensorctl                            # 10 live readings
sensorctl -s                         # daemon stats
sensorctl -r 200                     # sampling rate -> 200 ms
sensorctl -t 23 -S                   # threshold 23C, wait for the pulse
```

`sensorctl` options: `-d device` (default `/dev/sensor0`), `-n count`
(default 10), `-r rate_ms` (set sampling rate), `-t thresh_C` (set
threshold), `-s` (print stats), `-R` (reset stats), `-S` (subscribe to
the threshold pulse and wait).

## Verified

- 2026-11-16: built with SDP 8.0 (`qcc` aarch64), IFS 2,662,468 bytes.
  Booted under QEMU 8.2.2 TCG (`-machine virt -cpu cortex-a57 -m 256`).
  Auto-test passes end to end: live readings (temp ~22.00C, hum ~44.5%,
  press ~1013.07 hPa), `devctl` SET_RATE to 100 ms confirmed,
  threshold pulse fired (`THRESHOLD CROSSED: temp=23.13C`), stats
  `reads=7 devctls=4 pulses=1 rate=100ms threshold=23.00C`.
- Demo video: 92.3 s, 1080p (see the hackathon submission; the file
  `demo.mp4` sits next to this repo in the submission folder).

## Notes for builders

- The `[search]` paths in `build/sensorhub.build` point at the SDP
  install used here (`/home/hatch/qnx/qnx800/...`) — adjust them to your
  own SDP install path.
- The runtime linker is the real `ldqnx-64.so.2` binary (in the target
  tree's `usr/lib`), **not** a symlink to `libc.so.6` (`libc.so.6` has
  `e_entry` 0x0; symlinking it makes every dynamic process die with
  SIGILL). The buildfile embeds the real linker.
- There is no `/bin/ls` anywhere in the SDP 8.0 target tree — the image
  uses `ksh` globbing (`echo /dev/sensor*`) instead.
- `startup-qemu-virt` is the BSP startup binary for QEMU virt (aarch64);
  the image address is `0x40200000` — QEMU reserves a 1 MB DTB region at
  RAM base `0x40000000` and silently drops the DTB if the image overlaps
  it, which leaves `startup` parked in a `wfi` loop with no FDT.

## License

MIT — see LICENSE.
