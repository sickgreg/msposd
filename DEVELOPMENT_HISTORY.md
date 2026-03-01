# msposd2 Development History

This document tracks how `msposd2` was created from `msposd` and must be updated when new features or behavior changes are added.

## Purpose
- Keep a clear, chronological record of design and implementation decisions.
- Make it easy to understand why `msposd2` differs from `msposd`.
- Provide a stable reference for runtime behavior, especially UART modes.

## Scope of msposd2
- Target SoC family: Star6E (`ssc338q` / `ssc30kq`).
- Operating model: local renderer only.
- Input path: serial MSP from FC (`--master`, `--baudrate`).
- OSD behavior: always enabled by default.
- Message overlay file: `/tmp/MSPOSD.msg`.

## Chronology

### 2026-02-27: Initial msposd2 creation
- Created `msposd2/` as a stripped branch of the existing `msposd` source tree.
- Kept required renderer/OSD components:
  - `bmp/`, `libpng/`, `osd/`, `fonts/`, `sdk/infinity6/`
  - `compat.c`, `osd.c`, `osd.h`, `msposd.h`
- Added new entrypoint: `msposd2.c`.
- Added Star6E-focused build file: `msposd2/Makefile`.
- Added project readme: `msposd2/README.md`.

### 2026-02-27: Functional behavior alignment
- Set OSD mode on by default in `msposd2.c` (`DrawOSD = true`).
- Limited operation to local serial rendering:
  - Removed UDP/MAVLink runtime path from CLI and runtime flow.
  - Enforced serial device path validation for `--master`.
- Preserved Betaflight DisplayPort rendering pipeline by reusing `osd.c`.
- Preserved `/tmp/MSPOSD.msg` rendering behavior via existing `osd.c` implementation.

### 2026-02-27: Builder/toolchain integration and build fixes
- Integrated OpenIPC builder toolchain usage from `~/builder`.
- Added `DRV` library path support in `msposd2/Makefile` for SigmaStar libraries.
- Fixed duplicate symbol by referencing external `recording_dir`.
- Added `get_current_time_ms()` compatibility symbol needed by VTX/menu code.
- Verified cross-build output: ARM Star6E binary `msposd2/msposd2`.

### 2026-02-27: UART performance mode expansion
- Added explicit UART mode model:
  - `UART_MODE_EVENT` (default event-driven)
  - `UART_MODE_SIMPLE` (poll once per tick)
  - `UART_MODE_DRAIN` (poll and drain serial queue each tick)
- Added shared serial byte processing path for consistent stats and MSP parsing.
- Added new `-r 2xxx` mapping for high-throughput drain mode.
- Updated usage documentation accordingly.

### 2026-02-27: Low-rate polling support for 1xxx/2xxx
- Relaxed FPS floor for polling UART modes:
  - `1xxx` (simple poll) now supports `1..50` Hz.
  - `2xxx` (drain poll) now supports `1..50` Hz.
- Kept event mode (`-r N` without mode prefix) minimum at `5` Hz to preserve legacy behavior.
- Example low-rate settings:
  - `-r 1001` => simple poll at 1 Hz
  - `-r 2001` => drain poll at 1 Hz

## UART mode reference

### `-r 30`
- Mode: `UART_MODE_EVENT`
- Behavior: event-driven UART read via libevent bufferevent callbacks.
- Effective poll/refresh rate: `30` Hz.
- Best for: normal operation with balanced CPU usage.

### `-r 1010`
- Mode: `UART_MODE_SIMPLE`
- Behavior: poll timer mode; reads serial once each tick.
- Effective poll/refresh rate: `10` Hz (`1010 % 1000`).
- Allowed range: `1..50` Hz (`1001..1050` equivalent).
- Best for: low-rate testing and compatibility with previous `1xxx` behavior.

### `-r 2010`
- Mode: `UART_MODE_DRAIN`
- Behavior: poll timer mode; repeatedly reads until queue is drained (or loop cap).
- Effective poll/refresh rate: `10` Hz (`2010 % 1000`).
- Allowed range: `1..50` Hz (`2001..2050` equivalent).
- Best for: higher UART burst tolerance and reduced backlog risk.

## DMA note
- True UART DMA improvements are primarily kernel/driver-level work, not a pure userspace switch in `msposd2`.
- `msposd2` currently improves userspace ingestion strategy (drain mode), not kernel DMA configuration.

## Update rule (keep this file current)
When adding or changing features, append a new dated section under `Chronology` with:
- What changed.
- Why it changed.
- Runtime impact (CLI/behavior/performance).
- Any compatibility notes.
