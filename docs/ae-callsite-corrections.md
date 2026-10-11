# Finding a callsite by its neighbours instead of by id

Two AE sites in this port were wrong in a way no id lookup could reveal, and
both were fixed the same way: by matching the code either side of the call.

## RunActorUpdates — owner was a dead duplicate

`REL::ID(556439, 2227608)` resolves on AE to `0xbd65a0`, which nothing in the
image calls or points at. The live copy is `0xbd6850`, and once a temporary rva
override proved it, the log named its real id: **2227611**, one past the dead
one. The call this hook replaces sits at `+0xfc` there rather than `+0xf0`,
because the live copy has the null guards optimised out.

Full story in `dead-duplicate-owner.md`.

## HandlePlayerItem — wrong callee, wrong offset

The owner (`TESObjectREFR::AddInventoryItem`, `REL::ID(78185, 2200949)`) was
right. Everything else about the AE entry was not.

OG calls the callee (id 357079, rva `0xae1160`) **exactly once** inside the
owner, at `+0xa40`. AE calls the id taken from AnimatedWorldFramework PR #1
(2194003, rva `0x3477c0`) **three** times, at `+0x5cb`, `+0x825` and `+0xa4a` —
which is what produced the `pattern, 3 match(es)` warning every AE run. The
fixed offset `0xa4a` landed on the third one.

But that callee is a different function. Every AE call to it looks like:

```
lea  rdx,[rbp-0x54]
mov  r8,r13
lea  rcx,[rsp+0x78]
call 0x3477c0
```

Two stack addresses and a pointer. The hook expects `(TESBoundObject* item,
ExtraDataList* extras, uint32 count)`. So on AE this hook has been replacing an
unrelated call with a mismatched signature since the port began.

### How the real site was found

Not by ids — by the instructions around it. On OG:

```
cmp   r12,[rip]            ; PlayerCharacter singleton
jne   ...
mov   r8d,[rbp+0xd8]       ; count
mov   rdx,[r13]            ; extras
mov   rcx,rdi              ; item
call  0xae1160             ; <- the site, +0xa40
cmp   byte [rdi+0x1a],0x2c ; form type
```

Searching the AE owner for the same bracket — a comparison against the player
singleton, then a `cmp byte [item+0x1a],0x2c` a few instructions later — finds
exactly one region, and the call inside it is:

```
cmp   rdi,[rip]            ; PlayerCharacter singleton (0x1431ede50)
jne   ...
mov   r8d,[rbp+0x118]      ; count
mov   rcx,r13              ; item
mov   rdx,[r12]            ; extras
call  0xa36880             ; <- the real site, +0xB17
cmp   byte [r13+0x1a],0x2c ; form type
```

Same three arguments, same guard before, same test after. Both brackets are
unique in their image. AE offset is now `0xB17`; the callee id is unknown and
left empty so the next dump names it. NG's offset came from the same bad
source and is cleared — the site fails closed there rather than writing into
an unverified call.

## AddAcquiredEvent checked the same way and is correct

OG `+0x2d6` and AE `+0x354` are instruction-for-instruction the same:
`cmovne eax,ecx` / `mov r9,[rsp+0x40]` / `mov r8,…` / `mov rcx,[player
singleton]` / `mov rdx,…` / `mov [rsp+0x20],eax` / `call`, with an identical
continuation. That pairing, and callee id 2232930, stand.

## The lesson

An Address Library id says *where a function is*, never *which call this mod
means* or *whether anything runs it*. Three separate AE failures in this port
came from trusting an id alone:

1. an id naming a dead duplicate (`RunActorUpdates`),
2. an id naming an unrelated function that happened to be callable from the
   right owner (`HandlePlayerItem`),
3. an id naming a real function with a different argument order
   (`PlayIdleAction`).

The check that catches all three is the same: read the callee's prologue and
the caller's code either side of the call, and require them to match the
runtime that already works.

## Postscript: the ids are all settled

The AE dump named the last unknown callee — `rva 0xa36880 = id 2222424`, called
exactly once inside the owner, so `AUTO_CALLSITE` resolves it cleanly and the
`pattern, 3 match(es)` warning is gone. Every AE site now resolves by id:

| Site | AE owner | AE callee |
| --- | --- | --- |
| RunActorUpdates | 2227611 | 2228929 |
| AddAcquiredEvent | 2221653 | 2232930 |
| ActivateRef | 2233039 | 2201147 |
| HandlePlayerItem | 2200949 | 2222424 |
| UseObject | 2231392 | 2231415 |
| SetInputDeviceLightState | 2233201 | 2268396 |

Fixed interior offsets are now fallbacks on AE, not load-bearing.
