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
