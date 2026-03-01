# msposd2 (Star6E local renderer)

`msposd2` is a stripped-down Star6E (`ssc338q` / `ssc30kq`) build focused on local OSD rendering only.

See [DEVELOPMENT_HISTORY.md](./DEVELOPMENT_HISTORY.md) for the running change log and design history from `msposd` to `msposd2`.

## What it keeps
- Betaflight MSP DisplayPort rendering (same rendering pipeline as `msposd`)
- Serial input from flight controller (`--master`, `--baudrate`)
- OSD enabled by default (no need to pass `--osd`)
- `/tmp/MSPOSD.msg` message rendering
- AHI, matrix selection, and optional `--mspvtx`
- Startup video sizing via `-z WxH`

## What it removes
- UDP input/output forwarding modes
- MAVLink mode
- Ground-station relay behavior

## How It Differs From `msposd`
- Main OSD rendering uses incremental redraws when possible instead of rebuilding the full canvas every frame.
- `MSPOSD.msg` is throttled separately from the main render path and only rerasterizes on content changes or dynamic token refreshes.
- Serial polling, outgoing MSP requests, OSD redraw cadence, and message refresh cadence can be controlled independently (`--serial-hz`, `--msp-hz`, `--render-hz`, `--msg-hz`).
- On SigmaStar direct-canvas builds, a periodic full-canvas resync runs every 3 seconds to recover from stale or partially initialized overlay memory.
- On SigmaStar shutdown, explicit `MI_RGN_*` teardown is skipped to improve restart stability.

## Build (Star6E)
```sh
PATH=$HOME/builder/openipc/output/host/bin:$PATH \
make -C msposd2 star6e \
  CC=arm-openipc-linux-gnueabihf-gcc \
  TOOLCHAIN=$HOME/builder/openipc/output/host/opt/ext-toolchain/arm-openipc-linux-gnueabihf/sysroot \
  DRV=$HOME/builder/openipc/output/target/usr/lib
```

The output binary is:
- `msposd2/msposd2`

## Usage
```sh
msposd2 --master /dev/ttyS2 --baudrate 115200 --matrix 11 --ahi 3 -r 30
```

Compatibility note: `--osd` is still accepted but is always on in `msposd2`.

Independent rate controls:
- `--serial-hz N`: serial polling cadence for `1xxx` / `2xxx` UART modes
- `--msp-hz N`: outgoing MSP request cadence
- `--render-hz N`: max local OSD redraw cadence
- `--msg-hz N`: `/tmp/MSPOSD.msg` and text refresh cadence
- `-z WxH`: force the video size used for OSD layout at startup (for example `-z 1920x1080`)

Example with decoupled rates:
```sh
msposd2 --master /dev/ttyS2 --baudrate 115200 -r 1010 \
  --msp-hz 4 --render-hz 4 --msg-hz 2
```

Example low-load Star6E profile:
```sh
msposd2 --master /dev/ttyS2 --baudrate 115200 -r 1009 \
  --msp-hz 3 --render-hz 3 --msg-hz 3 -z 1920x1080
```

UART mode note:
- `-r 30`: normal event-driven UART mode
- `-r 1010`: simple poll UART mode at 10Hz (`1xxx` supports `1..50`, e.g. `1001` = 1Hz)
- `-r 2010`: high-throughput UART drain mode at 10Hz (`2xxx` supports `1..50`, e.g. `2001` = 1Hz)

## Render Behavior
- Main OSD rendering uses incremental redraws when possible instead of rebuilding the entire canvas every frame.
- `MSPOSD.msg` updates are throttled separately from the main render rate and only rerasterize on content changes or dynamic token refreshes.
- The direct SigmaStar canvas path performs a periodic full-canvas resync every 3 seconds to recover from stale or partially initialized overlay memory.
- `-z` only affects startup layout. If the video resolution changes later, restart the process so the OSD picks up the new size.

## Shutdown Behavior
- On SigmaStar builds, shutdown intentionally skips explicit `MI_RGN_*` teardown calls.
- Instead of calling `MI_RGN_DetachFromChn()`, `MI_RGN_Destroy()`, or `MI_RGN_DeInit()` during exit, the process waits briefly (`500 ms`) and exits.
- This is deliberate: repeated restart testing showed that touching SigmaStar RGN teardown APIs during process exit made restart stability worse than leaving cleanup to the platform.
- If your integration restarts `msposd` on resolution changes, this "do less on exit" behavior is the recommended path.
