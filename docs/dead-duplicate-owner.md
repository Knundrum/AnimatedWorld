# AE ships the per-frame driver twice, and the Address Library id points at the dead one

## Symptom

On AE 1.11.240 everything that fires from its own hook worked — activate, equip,
Pip-Boy light — while the item-added animation never played. On OG the same
build was fine. The log showed the hook installing cleanly:

```
RunActorUpdates: callsite discovered automatically at +0xf0
installed RunActorUpdates hook
```

and then **not one line** from the hook body afterwards, including the
one-shot lines that report each of its early exits. A hook that installs, is
verified by `REL::AUTO_CALLSITE`, and never executes.

## Cause

`REL::ID(556439, 2227608)` resolves on AE to rva `0xbd65a0`. Scanning the whole
`.text` for `E8`/`E9` branches to that address, and the whole image for a
qword equal to its VA:

```
direct calls to 0xbd65a0: 0
direct jmps  to 0xbd65a0: 0
qword pointer refs:       0
```

Nothing reaches it. It is a cold duplicate the linker left in the image.

The live copy is **`0xbd6850`**, reached through a function pointer at
`0x2f30fc0`. It is the same function with the null guards optimised out —
same singleton sequence, same `call 0xc34eb0` then `call 0xc34fa0`, same
`[rax+0xb8]` check afterwards — which shifts the call this mod hooks from
`+0xf0` to **`+0xfc`**.

For comparison, OG's copy at `0xcbe2c0` has exactly one caller and no
duplicate.

## Why AUTO_CALLSITE did not catch it

`REL::AUTO_CALLSITE` proves that a call to the expected callee exists at some
offset inside the owner. It cannot prove the owner is *executed*. Both copies
contain a real call to id 2228929, so the discovery succeeded inside the dead
one and reported a plausible `+0xf0`.

That is the general limit: callsite discovery validates the *shape* of a
function, never its reachability.

## Fix

`HookSite` gained an optional per-family raw owner rva, used here for AE only.
A raw rva is pinned to one build, so it is honoured only when the branch it
selects actually targets the site's `callsiteTarget`; otherwise the site fails
closed and says so. When it is accepted, the log reports the Runtime Database
id that owns that rva, so the override can be replaced by an ordinary id:

```
RunActorUpdates: owner rva 0xbd6850 = id <n> - replace the rva with this id
```

## The other five owners are fine

Same scan, AE 1.11.240:

| Site | AE owner rva | callers |
| --- | --- | --- |
| RunActorUpdates | `0xbd65a0` | **0 — dead** |
| AddAcquiredEvent | `0xa0a5c0` | 1 |
| ActivateRef | `0xd69b80` | 2 |
| HandlePlayerItem | `0x502c70` | 34 |
| UseObject | `0xce5e20` | 54 |
| SetInputDeviceLightState | `0xd76930` | 7 |

## Worth reusing

Before trusting any hook owner on a new runtime, check that something reaches
it. Two scans over the mapped image answer it in seconds: relative `E8`/`E9`
branches whose target is the function, and an 8-byte search for its absolute
VA to catch vtable and callback-table entries. A function with neither is not
one the game runs, whatever the Address Library says.
