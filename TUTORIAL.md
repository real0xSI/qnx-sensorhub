# QNX SensorHub — step-by-step tutorial

Build a QNX 8.0-native **virtual-sensor resource manager** from scratch, boot it
under QEMU, and watch a threshold crossing wake a client with a **pulse** instead
of a poll. No QNX hardware needed — everything runs on your Linux host.

- **Time:** ~45 minutes (plus SDP install time)
- **Level:** intermediate C / embedded. You should be comfortable with a shell,
  a Makefile, and the idea of a device file.
- **Result:** a bootable QNX image (IFS) that exposes `/dev/sensor0`
  (temperature) and `/dev/sensor1` (humidity + pressure), a `sensorctl` client
  that configures the daemon with `devctl()`, and a threshold pulse that fires
  when the temperature crosses 23 °C.

> **Verification status.** Steps 3–4 (compile + IFS packaging) were re-verified
> on 2026-11-18 with SDP 8.0 — see [Verification](#verification). Steps 5–6
> (shim rebuild + QEMU boot) stand on the original verified run of 2026-11-16
> (exact numbers below); they were **not** re-run on 2026-11-18 because the
> verification machine had no QEMU installed. Nothing in this tutorial is
> invented: every command comes from the repo's own README, build files, and
> scripts.

---

## What you will learn

1. The QNX **resource-manager framework** (`resmgr_attach`, `dispatch_create`,
   `iofunc_*`) — how a userspace daemon becomes a device file.
2. `devctl()` done right: command codes must use the `__DIOF` / `__DIOT` /
   `__DION` macros so the framework knows the data direction and size.
   Plain integers deliver no data (`nbytes 0` → `EINVAL`).
3. **QNX pulses** for event notification: the daemon sends a pulse into a
   client channel the moment a threshold is crossed — the supervisor wakes
   immediately instead of discovering the crossing on the next poll.
4. Packaging a bootable IFS with `mkifs` and booting it under QEMU — including
   the two traps that silently hang the boot (the DTB region and the ELF
   `-kernel` loader).

## Prerequisites

| Item | Notes |
|---|---|
| QNX SDP 8.0 | Installed via QNX Software Center. A **non-commercial / academic license is enough** to build and boot this. |
| Linux host | The SDP 8.0 toolchain on `PATH` after sourcing `qnxsdp-env.sh`. |
| `qemu-system-aarch64` 8.x | For the boot test. TCG is fine — **no KVM needed**. |
| `git`, `make` | Standard host tools. |

---

## Step 1 — Clone the repo and tour it

```sh
git clone https://github.com/real0xSI/qnx-sensorhub
cd qnx-sensorhub
```

| Path | What it is |
|---|---|
| `src/sensorhub.c` | The resource-manager daemon (~11 KB). Registers the devices, serves `read()` and `devctl()`, runs a POSIX timer, sends the threshold pulse. |
| `src/sensorctl.c` | The client (~6.5 KB). Reads live values, sends `devctl()` commands, subscribes to the pulse and waits for it. |
| `src/sensorhub.h` | Shared definitions: device paths, `devctl` command codes, the subscribe/stats structs, pulse code. |
| `src/sleep.c` | Minimal `/bin/sleep` helper — the SDP target tree ships no `sleep`/`echo`. |
| `build/Makefile` | Builds `sensorhub` + `sensorctl` with `qcc` for `aarch64` (default) or `x86_64`. |
| `build/sensorhub.build` | The `mkifs` buildfile: bootstrap, boot script, libraries, binaries. |
| `scripts/build-ifs.sh` | Runs `mkifs` → `out/aarch64/sensorhub.ifs`. |
| `scripts/boot-qemu.sh` | Boots the image under QEMU (256 MB guest, console on stdio). |
| `out/aarch64/shim5.s`, `shim5.ld` | Sources for the boot shim (see Step 5). |

## Step 2 — Understand the design

The daemon registers two devices in the QNX pathname space:

- `/dev/sensor0` — temperature, simulated with a slow upward drift from ~22 °C
- `/dev/sensor1` — humidity (~44.5%) + pressure (~1013 hPa), simulated

Reading a device is an ordinary `read()` — the resource manager serves it
through the `iofunc_*` layer. Configuration goes through `devctl()`:

| `devctl` command | Direction | Meaning |
|---|---|---|
| `DCMD_SENSOR_GET_RATE` | out (`__DIOF`) | Current sampling period, ms |
| `DCMD_SENSOR_SET_RATE` | in (`__DIOT`) | Set sampling period, ms |
| `DCMD_SENSOR_GET_STATS` | out (`__DIOF`) | `sensor_stats_t`: reads, devctls, pulses, rate, threshold, last temp/hum/press |
| `DCMD_SENSOR_RESET_STATS` | none (`__DION`) | Zero the counters |
| `DCMD_SENSOR_GET_THRESH` | out (`__DIOF`) | Current temperature threshold, °C |
| `DCMD_SENSOR_SET_THRESH` | in (`__DIOT`) | Set temperature threshold, °C |
| `DCMD_SENSOR_SUBSCRIBE` | in (`__DIOT`) | Register for the threshold pulse: passes the subscriber's pid, channel id, pulse code, and threshold |

The pulse path is the point of the project. A client calls
`DCMD_SENSOR_SUBSCRIBE` with its channel id; a POSIX timer in the daemon ticks
the sampling loop; the moment the temperature crosses the threshold, the daemon
sends a pulse (`SENSOR_PULSE_CODE`, i.e. `_PULSE_CODE_MINAVAIL + 1`) into the
client's channel via a `ConnectAttach(_NTO_SIDE_CHANNEL)` connection. The
client wakes the instant the crossing happens — it never polls.

> **Why a pulse and not polling?** The pattern this demonstrates is the one
> safety monitoring needs: a sensor crosses a threshold and the supervisor is
> woken by a pulse immediately, instead of discovering it on the next poll.
> Deterministic notification is the reason to build this on a real-time OS
> at all.

## Step 3 — Build the binaries

```sh
source <sdp-install>/qnxsdp-env.sh   # sets QNX_HOST, QNX_TARGET
make -C build                        # aarch64 (QEMU virt); or: make ARCH=x86_64

# Minimal /bin/sleep helper (the SDP target tree ships no sleep/echo):
qcc -Vgcc_ntoaarch64le -O2 -o out/aarch64/sleep src/sleep.c
```

Expected output: `out/aarch64/sensorhub` and `out/aarch64/sensorctl`. Two
compiler warnings are normal and harmless: a sign-compare warning in
`sensorhub.c` (`io_read`) and a deprecation warning for
`iofunc_devctl_verify` (the API still works in SDP 8.0).

## Step 4 — Package the bootable IFS

```sh
scripts/build-ifs.sh                 # -> out/aarch64/sensorhub.ifs
```

What happens inside:

- `mkifs` (`$QNX_HOST/usr/bin/mkifs` — **not** `mkqnximage`, which is the
  VM-image generator with different syntax) reads `build/sensorhub.build`.
- The buildfile's `[image=0x40200000]` address is deliberate: QEMU reserves a
  1 MB DTB region at RAM base `0x40000000–0x40100000` for ELF `-kernel` boots
  and silently **drops the DTB if the image overlaps it** — the startup then
  parks in a `wfi` loop with no FDT. An image at `0x40080000` boots to
  silence; at `0x40200000` it boots.
- The runtime linker embedded in the image is the **real `ldqnx-64.so.2`
  binary** from the target tree's `usr/lib` — not a symlink to `libc.so.6`
  (`libc.so.6` has `e_entry` 0x0; symlinking it makes every dynamic process
  die with SIGILL).
- The image contains `startup-qemu-virt`, `procnto-smp-instr`, the PL011
  serial driver, `ksh`, `pidin`, `slay`, the `sleep` helper, and the two
  binaries. There is **no `/bin/ls`** anywhere in the SDP 8.0 target tree —
  the image uses `ksh` globbing (`echo /dev/sensor*`) instead.

**Adjust one thing for your machine:** the `[search]` paths in
`build/sensorhub.build` point at the SDP install used here
(`/home/hatch/qnx/qnx800/...`). Point them at your own SDP install path
before running `mkifs`.

The resulting IFS is 2,662,468 bytes.

## Step 5 — Build the boot shim

QEMU's ELF `-kernel` loader does **not** pass the device-tree blob in `x0`
(and `-dtb` is ignored for ELF boots), while `startup-qemu-virt` needs the
FDT for RAM, UART, and GIC — without it, startup parks in a `wfi` loop,
silent. The shim (`out/aarch64/shim5.elf`) fixes that: it embeds the IFS,
puts the DTB at the top of RAM (`0x4f000000`), sets `x0` to it, and jumps to
the IFS entry.

From `out/aarch64/`, with the SDP environment sourced, using the **SDP
aarch64 tools** (bare `as`/`ld` on your host are the wrong architecture):

```sh
cd out/aarch64
ntoaarch64-as shim5.s -o shim5.o
ntoaarch64-ld -T shim5.ld shim5.o -o shim5.elf
```

The shim embeds two blobs (see `shim5.s`): `sensorhub.ifs` and the QEMU
virt device tree (`/tmp/virt-fixed.dtb` — dump it from QEMU's `virt` machine
first, e.g. with QEMU's `dumpdtb` option).

> **Rebuild the shim after any IFS change** — it embeds the IFS via `.incbin`,
> so a stale shim boots a stale image.

> ⚠️ **Not re-verified 2026-11-18.** The shim assembly was checked with the
> SDP assembler (valid aarch64; the only failure was the missing
> `/tmp/virt-fixed.dtb`, a QEMU-dumped artifact that is not in the repo and
> could not be regenerated here because the verification machine had no
> QEMU). The full shim + boot chain stands on the verified run of
> 2026-11-16. If you rebuild the shim, confirm `shim5.elf` exists before
> booting.

## Step 6 — Boot under QEMU

```sh
scripts/boot-qemu.sh                 # 256 MB guest, console on stdio
```

The exact QEMU invocation is:

```sh
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 256 \
    -kernel out/aarch64/shim5.elf -serial stdio -display none
```

Exit the guest with `Ctrl-A X`.

The IFS `.script` runs an **auto-test on boot** (starts `sensorhub`, reads
both devices, changes the rate to 100 ms, fires the threshold pulse, prints
stats) and then drops to an interactive `ksh` shell.

Expected auto-test output (verified 2026-11-16):

```text
qnx-sensorhub demo — QNX 8.0 on QEMU virt
serial console up — auto-test running, then interactive shell
auto-test: starting sensorhub
auto-test: devices /dev/sensor0 /dev/sensor1
auto-test: sensorctl -n 3 (read /dev/sensor0)
auto-test: sensorctl -d /dev/sensor1 -n 2
auto-test: sensorctl -r 100 -n 2 (devctl SET_RATE)
auto-test: threshold pulse (set 23C, subscribe, wait)
THRESHOLD CROSSED: temp=23.13C
auto-test: sensorctl -s (devctl GET_STATS)
auto-test done — interactive shell below
```

The verified run reported: live readings temp ~22.00 °C, humidity ~44.5%,
pressure ~1013.07 hPa; `devctl` SET_RATE to 100 ms confirmed; threshold pulse
fired at temp 23.13 °C; stats `reads=7 devctls=4 pulses=1 rate=100ms
threshold=23.00C`.

## Step 7 — Drive it by hand

From the interactive `ksh` shell (remember: no `ls` — use `echo /dev/sensor*`):

```sh
sensorhub &                            # start the daemon (if not already running)
echo /dev/sensor*                      # devices are up

sensorctl                              # 10 live temperature readings
sensorctl -d /dev/sensor1 -n 2         # humidity + pressure
sensorctl -s                           # daemon stats (reads, devctls, pulses, rate, threshold)
sensorctl -r 200                       # sampling rate -> 200 ms (devctl SET_RATE)
sensorctl -t 23 -S                     # threshold 23 °C, subscribe and wait for the pulse
sensorctl -R                           # reset stats
```

`sensorctl` options: `-d device` (default `/dev/sensor0`), `-n count`
(default 10), `-r rate_ms` (set sampling rate), `-t thresh_C` (set
threshold), `-s` (print stats), `-R` (reset stats), `-S` (subscribe to the
threshold pulse and wait).

**The threshold demo, step by step:** the temperature drifts slowly upward
from ~22 °C. Set the threshold just above the current reading
(`sensorctl -t 23 -S`) and wait — within seconds the daemon's timer tick
sees the crossing and delivers the pulse. Your shell prints
`THRESHOLD CROSSED: temp=23.13C` (exact value varies with the drift). Then
`sensorctl -s` shows `pulses=1`. No polling loop was involved anywhere: the
client blocked on its channel and the pulse woke it.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `make` fails: `QNX_HOST is not set` | Source `qnxsdp-env.sh` first. |
| `mkifs` can't find binaries | Run `make -C build` first; `build-ifs.sh` expects `out/<arch>/sensorhub` and `sensorctl` to exist. |
| `mkifs` can't find target files | Fix the `[search]` paths in `build/sensorhub.build` to your SDP install. |
| Boot hangs silently, no output | Image address overlaps QEMU's 1 MB DTB region — keep `[image=0x40200000]`; and/or the shim is stale or missing the DTB (rebuild it after any IFS change). |
| Processes die with SIGILL at startup | The image embedded a symlink to `libc.so.6` as the runtime linker — use the real `ldqnx-64.so.2` binary (the buildfile already does). |
| `ls: not found` in the guest | Expected — there is no `/bin/ls` in the SDP 8.0 target tree. Use `echo /dev/sensor*` and `pidin`. |
| `mkqnximage` errors on the `.build` file | Wrong tool — use `mkifs` (`$QNX_HOST/usr/bin/mkifs`). |
| `as`/`ld` reject `shim5.s` | Bare `as` is your host's x86 assembler — use the SDP's `ntoaarch64-as` / `ntoaarch64-ld`. |
| Shim build fails on `/tmp/virt-fixed.dtb` | The QEMU virt DTB is not in the repo — dump it from QEMU's `virt` machine first (see Step 5). |

## Going further

- **Change the sensors:** the simulated values live in `src/sensorhub.c` —
  change the drift rate or add a third device (a new `/dev/sensorN` + a
  `devctl` command in `src/sensorhub.h`).
- **Real hardware:** the same resource manager builds for `x86_64`
  (`make ARCH=x86_64`); only the IFS/boot path is QEMU-`virt`-specific.
- **More pulses:** `DCMD_SENSOR_SUBSCRIBE` takes a threshold per subscriber,
  so several clients can watch different thresholds on the same device.

## Verification

| Step | Status |
|---|---|
| `make -C build` (qcc aarch64) | ✅ Re-verified 2026-11-18 — exit 0, `sensorhub` + `sensorctl` produced (2 warnings, harmless — see Step 3). |
| `scripts/build-ifs.sh` (mkifs) | ✅ Re-verified 2026-11-18 — `sensorhub.ifs` 2,662,468 bytes, byte-identical size to the 2026-11-16 verified build. |
| Shim rebuild (`shim5.elf`) | ⚠️ Assembly valid with SDP `ntoaarch64-as`; blocked on missing `/tmp/virt-fixed.dtb` (QEMU-dumped DTB, not in repo; no QEMU on the verification machine). |
| QEMU boot + auto-test | ⚠️ Verified 2026-11-16 only (QEMU 8.2.2 TCG, `-machine virt -cpu cortex-a57 -m 256`): auto-test passed end to end, numbers in Step 6. Not re-run 2026-11-18 (no QEMU installed on that machine). |

## Links

- Repo: https://github.com/real0xSI/qnx-sensorhub
- Hackathon submission (QNX Global Hackathon 2026, Low-level Software):
  https://devpost.com/software/qnx-sensorhub
- Demo video: see the submission page (YouTube, 1080p).

## License

MIT — see LICENSE.
