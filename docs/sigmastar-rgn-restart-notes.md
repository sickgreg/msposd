# SigmaStar RGN Restart Notes

This note captures the SigmaStar (`libmi_rgn` / `libmi_sys`) restart behavior observed while testing `msposd2` / `msposd` on Star6E-class targets.

## Symptom

When `msposd` was killed and restarted, or when the camera pipeline restarted during a resolution change, the serial console repeatedly showed RGN / SYS cleanup warnings during the client disconnect and reconnect window.

Typical sequence:

```text
client [pid] disconnected, module:sys
client [pid] disconnected, module:rgn
Apps need check buffer clear flow. Driver clear window handler buffer ...
MI_SYS_IMPL_MmaFree[7163]: find_allocation_by_pa fail phyAddr=...
Apps need check buffer clear flow. Driver clear region handler 9.
...
client [newpid] connected, module:rgn
MI_RGN_IMPL_GetAttr[1318] Handle not found.
MI_RGN_IMPL_GetDisplayAttr[1819] pstChnPort and handle not matched.
MI_RGN_IMPL_DetachFromChn[1696] pstChnPort and handle not matched.
client [newpid] connected, module:sys
```

In earlier, worse cases, restarts could also trigger a flood of:

```text
_MI_SYS_MMU_Callback ... Status=0x2 ... IsWrite=0
```

That MMU callback spam was the most severe failure mode and looked like stale read access during video pipeline reconfiguration.

## What Was Tested

Several shutdown strategies were tested on SigmaStar:

1. Explicit RGN teardown:
   - `MI_RGN_DetachFromChn()`
   - `MI_RGN_Destroy()`
   - `MI_RGN_DeInit()`
2. Staged teardown with short delays before destroy.
3. Hide-first attempts before teardown.
4. Detach-only / no-destroy variants.
5. No explicit RGN teardown at all.

### What did not help

- Explicit detach / destroy / deinit on process exit consistently reproduced the cleanup warnings above.
- Reusing `MI_RGN_AttachToChn()` as a fake "hide" operation made things worse and caused:

```text
MI_RGN_IMPL_AttachToChn ... Channel port had been attached.
```

- Longer delays before `MI_RGN_Destroy()` reduced some symptoms but did not eliminate the recurring RGN / SYS cleanup errors.
- Even skip-destroy / skip-deinit variants did not fix the issue if detach was still attempted.
- Using the real `MI_RGN_SetDisplayAttr()` to hide before exit did not materially change the repeated disconnect/reconnect errors.

### What helped

The most stable behavior came from doing less:

- Skip all explicit SigmaStar `MI_RGN_*` teardown calls during shutdown.
- Do not call:
  - `MI_RGN_DetachFromChn()`
  - `MI_RGN_Destroy()`
  - `MI_RGN_DeInit()`
- Instead, wait briefly and let the process exit.

The current chosen behavior is:

- On SigmaStar shutdown, `msposd` waits `500 ms` and exits.
- No explicit RGN teardown is attempted from userspace.

This does not eliminate all SDK warnings on client disconnect, but it materially improved restart stability and avoided the worst MMU-fault flood seen with more aggressive teardown.

## Current Interpretation

The evidence suggests the core trigger is the SigmaStar RGN client disconnect when the process exits, not normal rendering and not any single explicit teardown API.

In practice:

- "Cleaning up properly" through explicit RGN teardown APIs made restart stability worse.
- A conservative "touch the SDK as little as possible on exit" policy was the safest option found in testing.

## Practical Recommendation

If your integration must restart `msposd` on resolution changes:

- Prefer the no-teardown SigmaStar shutdown path.
- Avoid adding explicit `MI_RGN_*` cleanup during exit unless the platform SDK behavior changes.
- Treat the serial warnings above as known platform behavior around process exit / module reconnect, not necessarily a userspace rendering bug.

## Later Findings

Additional testing on the same platform narrowed the failure down further.

### Single-port RGN works for live OSD

- Attaching the OSD region only to VPE output port `0` still produced visible live OSD.
- The second attach to VPE output port `1` does not appear to be required for the main live overlay path.
- The likely tradeoff is that JPEG snapshot OSD may be lost, since port `1` appears to feed the JPEG path.

### Explicit teardown still failed even with one port

- Reverting to a clean one-port reverse-order teardown (`Detach -> Destroy -> DeInit`) still caused freezes and visible corruption.
- In practice, switching from two-port teardown to one-port teardown did not make explicit shutdown safe enough.

### The strongest failure signal is a kernel BUG on process exit

Serial capture eventually showed that the most severe failure is not only user-space warnings. During `msposd` process exit, the kernel can BUG inside the SigmaStar driver stack:

```text
[MI_SYS_IMPL_MmaFree][7155]Case CamOsAtomicRead(&tmp->ref_cnt) != 0 BUG ON!!!
...
[<bfbe96f4>] (MI_SYS_IMPL_MmaFree [mi_sys])
...
[<bfc65971>] (_mi_rgn_drv_misys_buf_del [mi_rgn])
...
[<bfc62601>] (MI_RGN_IMPL_DeInit [mi_rgn])
...
[<bfc68483>] (mi_rgn_process_exit [mi_rgn])
...
[<bfbdc3cd>] (MI_DEVICE_Release [mi_common])
...
[<c0097b5b>] (__fput)
...
[<c001e471>] (do_exit)
```

This is the most important finding so far:

- the failure can happen during file release on process exit
- it is not limited to explicit `MI_RGN_DetachFromChn()` / `MI_RGN_Destroy()` calls from user space
- the driver can still blow up while the `mi_rgn` device is being released automatically

### Live `msposd` really holds the SigmaStar device nodes open

On a clean running instance, `/proc/<pid>/fd` showed:

- `3 -> /dev/mi_rgn`
- `4 -> /dev/mi_sys`
- `5 -> /dev/ttyS2`

This confirms that the RGN and SYS drivers are tied to process lifetime through open device fds, not only through explicit API calls.

### Timing-only exit tweaks did not solve it

The following variants were tested and did not materially fix the repeated restart problem:

- `500 ms` wait before exit
- `2000 ms` wait before exit
- immediate `_exit(0)` after the wait
- skipping UART close in the signal path

These tweaks changed the exact symptom timing, but repeated stop/start cycles still produced stuck `D` / `DW` state processes or driver faults.

## Current Direction

Because the kernel BUG is tied to `mi_rgn` device release during process exit, the stable direction is now:

- avoid process exit for resolution-change handling
- keep SigmaStar attached on one port only
- use `SIGHUP` for an in-process layout reload

This no longer uses the earlier soft-reexec experiment.

### Current `SIGHUP` behavior

`SIGHUP` now:

- rereads `/etc/majestic.yaml`
- extracts `video0.size`
- picks the largest built-in layout that fits the new frame (`HD` vs `FHD`)
- recomputes the OSD position from the new video size
- updates the existing RGN display position in place
- forces a redraw

It does **not** kill the process, detach the region, or re-open `/dev/mi_rgn`.

In the current implementation:

- same-bucket changes are applied live
- cross-bucket changes also switch live now by swapping to a second in-process region/font profile
- if the frame is too small for the chosen overlay, the OSD falls back to top-left placement instead of using a negative offset

### What was verified

Repeated in-process `SIGHUP` reloads were tested on-target and remained stable:

- PID stayed constant
- `/proc/<pid>/fd` stayed stable (`/dev/mi_rgn`, `/dev/mi_sys`, `/dev/ttyS2`, epoll, pipe)
- no extra `mi_rgn` / `mi_sys` fds accumulated
- no `client ... disconnected` sequence appeared
- no `MI_SYS_IMPL_MmaFree` kernel BUG was triggered

This was tested both:

- with repeated `SIGHUP` calls at the same configured size
- while toggling `video0.size` in `/etc/majestic.yaml` between `1104x816` and `1472x816`

Crossing into an `FHD`-class size was also tested:

- `1440x1080` (HD fit) -> `1920x1080` (FHD fit) -> `1440x1080`

This remained stable too:

- same PID
- stable fd table
- no process exit
- no `MI_SYS_IMPL_MmaFree` kernel BUG

During the Majestic reopen window, SigmaStar can still emit brief transient warnings such as:

```text
MI_RGN_IMPL_GetCanvasInfo ... Handle not found
```

Those happen while the old canvas disappears before the new one is fully ready, but the current renderer stays alive and recovers in place.

### What is no longer the recommended path

The earlier in-place re-exec experiment (`SIGHUP` preserving `/dev/mi_rgn` and `/dev/mi_sys` across `exec`) is no longer the active direction.

Why it was dropped:

- the first re-exec could avoid the kernel BUG
- but later re-execs either leaked extra `mi_*` fds or had to close them
- once the older `mi_*` fds were closed on a later `exec`, the process fell back into the same `MI_DEVICE_Release` / `MI_SYS_IMPL_MmaFree` crash path
- it also re-execed the same argv, so it did not naturally pick up a new runtime `-z` value

So the current recommendation is:

- use `SIGHUP` for live size/layout changes
- avoid kill/restart for size changes on SigmaStar whenever possible

See [agent-handover.md](./agent-handover.md) for the current live branch state and next-step testing notes.
