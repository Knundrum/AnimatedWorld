# Fallout 4 Address Library lookups needed — AnimatedWorld (F4SE plugin)

Context for whoever picks this up: this plugin currently ships working on **OG
1.10.163 only**. Every OG Address Library ID below is verified — the mod runs in
game with them. What is needed is the **same function's ID on NG (1.10.984) and
AE (1.11.x)**.

## How to read this — please read before looking anything up

**Anchor on the OG ID, not on the name.** Function names differ between
CommonLib forks and between people's Ghidra databases. The reliable procedure is:

1. Resolve the **OG ID** below against `version-1-10-163-0.bin` → OG RVA.
2. Open the OG `Fallout4.exe` at that RVA; that is the function, by definition.
3. Find the same function in the NG / AE executable (BinDiff or Diaphora against
   OG is the sane route; strings and xrefs otherwise).
4. Resolve its NG/AE RVA back to an ID via the corresponding Address Library bin.

**Please return the RVA next to every ID.** The ID alone cannot be checked; the
RVA can be verified against a real executable.

**Prototypes are given so the ABI can be sanity-checked.** If the function you
land on does not match the prototype, the mapping is wrong — say so rather than
returning it. Two of the prototypes below are inherited from an old CommonLibF4
fork and may themselves be wrong; those are flagged.

---

## Group A — functions the plugin calls directly

Five ordinary functions. Only the NG/AE **ID** is needed for each.

| # | OG ID | Prototype as called (x64, first arg is `this` where applicable) | Status |
|---|---|---|---|
| A1 | **1451490** | `bool (Actor* a_actor, BGSAction* a_action, TESObjectREFR* a_target, void* a_unk, uint32_t a_flags)` | NG + AE needed |
| A2 | **708895** | `void __fastcall (NiAVObject*, const BGSMaterialSwap*, float, float, void*)` | NG + AE needed — **prototype suspect, see Conflicts** |
| A3 | **407609** | `bool (TESObjectREFR* a_this)` — activation-blocked test | NG + AE needed |
| A4 | **900857** | `bool (TESObjectREFR* a_this, BGSKeyword* a_keyword)` — `WornHasKeyword` | AE **2200995** claimed, unverified; NG needed |
| A5 | **663900** | `void (PipboyManager* a_this, const BSFixedString& a_menuName)` — `PlayPipboyOpenAnim` | NG + AE needed |

---

## Group B — hook-site **owner** functions

This is the part that is easy to get wrong, so please read it carefully.

These six IDs are **not functions the plugin calls**. Each is a function that
*contains* a `call` instruction which the plugin replaces with a trampoline. So
each row has an **owner** (the containing function) and a **callee** (the
function being called at that offset). They are different functions.

For each row, **either** of these closes it — the second is strongly preferred:

- **(a)** the owner's NG/AE ID **and** the byte offset of the same call inside
  the NG/AE build, or
- **(b)** the owner's NG/AE ID **and** the callee's OG + NG/AE IDs.

Option (b) is better because CommonLibF4RD can then locate the call itself at
runtime via `REL::AUTO_CALLSITE(target)`, which survives the function body
shifting in a future patch. A hardcoded interior offset does not.

| # | Owner OG ID (OG rva) | Owner name | OG offset | Branch | **Callee OG ID** (OG rva) |
|---|---|---|---|---|---|
| B1 | **556439** (`0xcbe2c0`) | *unnamed* | `0xF0` | `E8` call | **1318162** (`0xd39e40`) |
| B2 | **1401485** (`0xa92810`) | *unnamed* | `0x2D6` | `E8` call | **876119** (`0xe9f220`) |
| B3 | **785533** (`0xead140`) | *unnamed* | `0x38A` | `E8` call | **753531** (`0x40c750`) = `TESObjectREFR::ActivateRef`, AE **2201147** — **closed except the owner ID** |
| B4 | **78185** (`0x3fbb00`) | `TESObjectREFR::AddInventoryItem`, AE **2200949** | `0xA40` | `E8` call | **357079** (`0xae1160`) |
| B5 | **988029** (`0xe1bcd0`) | `ActorEquipManager::EquipObject`, AE **2231392** | `0x15A` | `E8` call | **301794** (`0xe1d100`) |
| B6 | **520007** (`0xeb9380`) | `PlayerCharacter::TogglePipBoyLight`, AE **2233201** | `0x5B` | **`E9` jmp** | **157452** (`0x1b2c080`) |

The callee IDs and RVAs above were read off a live OG 1.10.163 session by
decoding the branch at each site and reverse-looking-up the target through the
Address Library. They are measured, not inferred.

**This changes the ask for Group B.** Interior offsets are no longer wanted.
For each row, the NG/AE IDs of **both** the owner and the callee are enough —
CommonLibF4RD then locates the call itself at runtime. Owner IDs for B4/B5/B6
are already known, so those three rows need only their **callee's** NG/AE ID.

Callee prototypes, for ABI confirmation:

| # | Callee prototype |
|---|---|
| B1 | `void __fastcall (void* a_this)` — per-frame actor update |
| B2 | `void __fastcall (PlayerCharacter*, TESBoundObject*, TESForm*, TESObjectREFR*, int32_t a_acquireType)` |
| B3 | `bool __fastcall (TESObjectREFR* a_this, TESObjectREFR* a_actionRef, TESBoundObject* a_objectToGet, int a_count, bool a_defaultProcessingOnly, bool a_fromScript, bool a_looping)` |
| B4 | `void __fastcall (TESBoundObject*, ExtraDataList*, uint32_t a_count)` |
| B5 | `bool __fastcall (ActorEquipManager*, Actor*, const BGSObjectInstance&, uint32_t a_stackID, uint32_t a_number, const BGSEquipSlot*, bool a_queueEquip, bool a_forceEquip, bool a_playSounds, bool a_applyNow, bool a_locked)` |
| B6 | `void __fastcall (BSInputDeviceManager*, uint32_t a_state, bool a_on)` |

Note on **B6**: the live dump confirms this site is an `E9` **jmp**, i.e. a tail
call, not a `call`. That matters for automatic callsite lookup — a search for a
call will not match it.

### Already known (no work needed)


| Owner | OG | AE | Source |
|---|---|---|---|
| B4 `TESObjectREFR::AddInventoryItem` | 78185 | **2200949** | CommonLibF4RD headers |
| B5 `ActorEquipManager::EquipObject` | 988029 | **2231392** | CommonLibF4RD headers |
| B6 `PlayerCharacter::TogglePipBoyLight` | 520007 | **2233201** | CommonLibF4RD headers |

### What is actually left in Group B

| Row | Owner NG/AE ID | Callee NG/AE ID |
|---|---|---|
| B1 | needed (`556439`) | needed (`1318162`) |
| B2 | needed (`1401485`) | needed (`876119`) |
| B3 | needed (`785533`) | **done** — `2201147` |
| B4 | done — `2200949` | needed (`357079`) |
| B5 | done — `2231392` | needed (`301794`) |
| B6 | done — `2233201` | needed (`157452`) |

So: three owner IDs and five callee IDs, eight lookups total for Group B.

---

## Conflicts to check

**1. `2201146` vs `2201147` — now with evidence.** CommonLibF4RD gives
`TESObjectREFR::ActivateRef` as `REL::ID(753531, 2201147)`, and the live OG dump
confirms `753531` is the callee at B3, at OG rva `0x40c750`. Separately,
`IsActivationBlocked` (A3, OG **407609**) sits at OG rva `0x40c6d0` — `0x80`
earlier, i.e. immediately before `ActivateRef` in the OG binary.

So the hypothesis is **`2201146` = `IsActivationBlocked`** and **`2201147` =
`ActivateRef`**: two adjacent functions with two consecutive IDs. That would
close A3 for free. It is a hypothesis, not proof — please confirm by checking
whether `2201146`'s target sits immediately before `2201147`'s in the NG/AE
executable, and that its prototype is `bool (TESObjectREFR*)`.

**2. `QueueApplyMaterialSwap` ABI.** A prototype has been reported as
`QueueApplyMaterialSwap(NiAVObject*, BGSMaterialSwap*, float, bool, bool)`. The
plugin (A2, OG **708895**) calls its target as
`(NiAVObject*, const BGSMaterialSwap*, float, float, void*)`. Those disagree on
arguments 4 and 5, and under the x64 calling convention that is a different
register layout — `1.0f` would land in `xmm3` where a `bool` is expected in
`r9b`. Either they are different functions, or the plugin's inherited prototype
is wrong. Please confirm what OG **708895** actually is and its true signature.

**3. RESOLVED — OG `1451490` is the `Actor.PlayIdleAction` Papyrus native.**
Settled by disassembly of both binaries; see `docs/playidleaction.md`. It is
the only function in either image referencing both `"Cannot run a None
action."` and `"Cannot run an action on a None actor"`. OG `1451490` =
rva `0x13865c0`; AE counterpart = rva `0x10fa960`, reported as ID `2251634`
(ID-to-rva link still unconfirmed locally). The argument order differs between
OG and AE. It is **not** `Actor::PerformAction` (OG `1057231` = rva `0xe04fb0`).
No further work needed on this item.

~~Is OG `1451490` really `MovementAgentPathFollowerStandard::PlayActionAtNode`?~~
That name has been associated with this work, but its reported prototype is
`(const PathFollowerStateUpdateData&, uint32_t)` — a different class and arity
from A1's `(Actor*, BGSAction*, TESObjectREFR*, void*, uint32_t)`. A1 works in
game on OG with the five-argument form, so if `1451490` is `PlayActionAtNode`
then that prototype needs re-examining; more likely they are different functions.

---

## Requested output format

Plain text or a table, one line per item, e.g.:

```
A1  1451490  ->  NG <id> (rva 0x......)  AE <id> (rva 0x......)   confirmed-prototype: yes/no
B1  556439   ->  NG <id> (rva 0x......)  AE <id> (rva 0x......)
    B1 callee ->  OG <id>  NG <id>  AE <id>        [preferred]
    B1 offset ->  NG 0x...  AE 0x...               [fallback]
```

Anything that cannot be closed confidently: please say so explicitly rather than
supplying a best guess. An ID in the wrong slot does not fail loudly — it
resolves to an unrelated function and the hook is written into the middle of it.

## Note on runtime families

CommonLibF4RD classifies 1.10.163 as OG, up to 1.10.984 as NG, and 1.11.x as AE.
A mapping proven only on 1.10.984 should be labelled as such; 1.10.980 is a
different executable and should not be assumed to share it.
