# PlayAction / PlayIdleAction — address and ABI, settled from the binaries

Status: **resolved by direct disassembly of both shipped executables.** This
supersedes the `Actor::PerformAction` pairing that was applied briefly and has
now been reverted.

## What the plugin calls

The engine function behind `AW::Game::PlayAction` is the Papyrus native for
`Actor.PlayIdleAction`. It is the only function in either binary that
references both of these strings:

- `"Cannot run a None action."`
- `"Cannot run an action on a None actor"`

## Identification

| Runtime | String RVAs | Referencing function | Address Library ID |
| --- | --- | --- | --- |
| OG 1.10.163 | `0x2d93b30`, `0x2d93b50` | `0x13865c0 .. 0x138674c` | **1451490** |
| AE 1.11.240 | `0x25b9c58`, `0x25b9c78` | `0x10fa960 .. 0x10faabd` | **2251634** |

Method: locate the two strings in the mapped image, scan `.text` for
rip-relative `lea` instructions targeting them, and resolve the enclosing
function through the `.pdata` exception table (exact function bounds, not a
prologue heuristic). Exactly two references were found per binary, both inside
the one function.

The OG side is anchored independently: `version-1-10-163-0.bin` maps
`1451490 -> 0x13865c0`, which is the function the strings point into, and
`1451490` is the ID this mod has used since before the port.

The AE ID `2251634` comes from a third party; the AE **RVA** `0x10fa960` was
confirmed here against the real 1.11.240 binary, but the ID-to-RVA link was
not, because no `version-1-11-240-0.bin` is present on this machine. That is
the one remaining unverified link in the chain.

## Why `Actor::PerformAction` is not this function

CommonLibF4RD declares `Actor::PerformAction` as `REL::ID(1057231, 2231177)`.
On OG, `1057231` resolves to RVA `0xe04fb0` — a different function, nowhere
near `0x13865c0`. Adopting `2231177` for this slot pointed the call at the
wrong AE function. Reverted.

## The parameter order differs between OG and AE

Same five values, same function, rotated argument list. Both binaries
null-check the actor and the action before touching the VM pointer, which is
why passing a null VM has always been safe — and why the null guards in
`Game::PlayAction` are load-bearing rather than defensive.

### OG `0x13865c0`

| Slot | Register | Value | Evidence |
| --- | --- | --- | --- |
| 1 | `rcx` | actor | `test rcx,rcx` → path emitting `"…None actor"` |
| 2 | `rdx` | action | `test rdx,rdx` → path emitting `"Cannot run a None action."` |
| 3 | `r8` | target | stored to `[rsp+0x20]`, the 5th slot of the inner call |
| 4 | `r9` | VM | `mov rbx,r9`; error paths do `mov rcx,rbx` / `call [rax+0x1e0]` |
| 5 | `[rsp+0x28]` | stack id | `mov r8d,[rsp+0xe0]` inside the frame, passed to the logger |

### AE `0x10fa960`

| Slot | Register | Value | Evidence |
| --- | --- | --- | --- |
| 1 | `rcx` | VM | `mov rbx,rcx`; error paths do `mov rcx,rbx` / `call [rax+0x1e0]` |
| 2 | `edx` | stack id | `mov edi,edx`, then `mov r8d,edi` into the logger |
| 3 | `r8` | actor | `test r8,r8` → path emitting `"…None actor"` |
| 4 | `r9` | action | `test r9,r9` → path emitting `"Cannot run a None action."` |
| 5 | `[rsp+0x28]` | target | `mov rax,[rsp+0xe0]` → `mov [rsp+0x20],rax` |

Both versions call the same logging slot `[vtable+0x1e0]` with the same
severity constant `mov r9d,2`, and both build the same `0xb0`-byte frame — the
same function, recompiled from changed source.

## Consequence in code

`Game.cpp` carries two function-pointer types and picks one by runtime family:

- OG: `bool(Actor*, BGSAction*, TESObjectREFR*, void* vm, uint32_t stackId)`
- NG/AE: `bool(void* vm, uint32_t stackId, Actor*, BGSAction*, TESObjectREFR*)`

**NG is inference, not measurement.** NG 1.10.984 is the Next-Gen recompile
that AE descends from, so it is assumed to use the AE order. No NG binary was
available to disassemble. A third party reports the NG entry for this ID as a
10-byte stub that conditionally jumps to `0x10749e3`; calling the stub is
fine, but if NG animations misbehave, the argument order is the first thing to
re-check.

## How to redo this check on a new runtime

1. Find `"Cannot run a None action."` in the unpacked executable.
2. Convert its file offset to an RVA with the section table.
3. Scan `.text` for `48/4c 8d /r` with `mod=00, rm=101` and a displacement
   landing on that RVA.
4. Map the hit to a function through `.pdata`.
5. Read the first four register uses and the `[rsp+0xe0]` read to recover the
   argument order before assuming it matches an existing family.
