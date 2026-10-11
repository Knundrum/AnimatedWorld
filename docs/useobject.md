# UseObject — why AUTO_CALLSITE found nothing on AE, and the real shape

## The site

`ActorEquipManager::EquipObject` is a thin wrapper that hands four arguments to
an inner worker:

```
rcx = ActorEquipManager*   rdx = Actor*
r8  = BGSObjectInstance*   r9  = pointer to a caller-built parameter block
```

The parameter block is assembled on the wrapper's own frame just before the
call (count, slot, and the equip flags), which is why it arrives as one pointer
rather than as separate arguments.

## OG has two workers; AE has one

**OG `0xe1bcd0`** (id 988029) calls two different functions:

| Offset | Callee | When |
| --- | --- | --- |
| `+0x15A` | `0xe1d100` (id 301794), size `0x13b` | fast path, taken when the equip is not blocked |
| `+0x196` | `0xe1eb50`, size `0x622` | slow path, after taking a lock, when the fast path returns false |

This hook has always taken the fast one, so an equip that falls through to the
slow path was never animated on OG. Pre-existing; left alone.

**AE `0xce5e20`** (id 2231392) merged them. Where OG branches to the fast
worker, AE has inlined checks instead, and every path converges on a single
four-argument worker call:

```
+0x283   e8 ..   call 0xce7fb0
```

That is the only four-argument worker call in the whole function. So the
`callsite_not_found, 0 match(es)` warning for id 2231408 on 1.11.240 was
correct and not a bad ID lookup: no call to any such function exists in AE's
`EquipObject`. `aeOffset = 0x283` is now set from this measurement, and the
first byte there is `0xe8` as required.

Side effect worth knowing: because AE's single site is on every path, the AE
hook covers equips that the OG hook misses. More animations on AE than OG, not
fewer.

NG is deliberately left unset — no NG binary was available to measure, and a
wrong interior offset writes a branch into the middle of an instruction.

## The signature was wrong everywhere, and worked anyway

`FnUseObject` declared eleven parameters
(`…, uint32 stackID, uint32 number, const BGSEquipSlot*, bool ×5`). The callee
takes four. On OG a genuinely four-argument callee just ignores the extra
register and stack slots, so nothing crashed — but the hook was reading those
eight values out of stack the caller never wrote, and passing garbage back down.
Now corrected to four on all runtimes. The hook only ever used the first three.

## New guard

`Addresses::ResolveSite` now checks the byte at `owner + offset` before handing
the address to the trampoline: `0xE8` for a call site, `0xE9` for a jump site.
A wrong fixed offset previously meant five bytes written into the middle of an
instruction and a crash somewhere unrelated; it now disables that one hook and
says so in the log. Every remaining unknown in this port is an interior offset,
so this is the cheapest safety net available.

## Still open

The AE id of `0xce7fb0` is unknown — the AE/NG `callsiteTarget` slots are empty.
The next AE callsite dump will print `UseObject call -> rva 0xce7fb0 = id …`,
because the dump decodes whatever site was actually resolved. Fill that id in
and the fixed offset stops mattering on AE.
