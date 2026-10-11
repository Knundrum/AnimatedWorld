# Reading a user's log

Every report starts with `Documents\My Games\Fallout4\F4SE\AnimatedWorld.log`.
Ask for the whole file, not an excerpt — the first twenty lines separate "wrong
game version" from "real bug" before any guessing starts.

## 1. The header

```
AnimatedWorld v1.0.0 loaded on runtime 1-11-240-0
AnimatedWorld address report - runtime 1-11-240-0 (family AE)
```

- **No log at all** — F4SE did not load the plugin. Wrong F4SE build for their
  runtime, or the DLL is not in `Data/F4SE/Plugins`.
- **Family NG (1.10.984)** — expect reduced function. `HandlePlayerItem` and
  `UseObject` have no NG interior offsets on purpose and will report
  "hook not installed"; the `PlayAction` argument order on NG is inferred from
  AE, never measured. An NG report is new territory, not a regression.
- **Any id line saying MISSING** — that runtime has no id for that function.
  Expected for anything outside OG/NG/AE.

## 2. Hook installation

Healthy AE and OG both look like this, one pair per site:

```
<site>: callsite discovered automatically at +0x...
installed <site> hook
```

- **`callsite unusable (pattern, N match(es))`** — several calls to that callee
  inside the owner; it fell back to the fixed offset. It then lists the
  candidates. Works, but the fixed offset is a guess about which one.
- **`callsite unusable (callsite_not_found, 0 match(es))`** — no call to that
  callee exists in that owner on this build. Either the callee id is wrong or
  the function was restructured. See `ae-callsite-corrections.md`.
- **`byte at +0x... is 0x..., expected 0xe8`** — the fixed offset does not point
  at a branch on their build. The hook is skipped rather than corrupting the
  function. Do not "fix" this by forcing it.
- **`no <family> interior offset supplied`** — deliberate. Fail-closed.
- **`RunActorUpdates hook unavailable`** — the plugin stays inactive; nothing
  else will work. Check the owner id first (`dead-duplicate-owner.md`).

## 3. Did it get running?

```
player animation graph event sink hooked (sink is +0x38 into PlayerCharacter)
forms resolved: activate=true itemAdded=true ...
[aw] driver live - state machine running
```

- **`forms resolved` missing** — the esp is not loaded or is renamed. It must be
  `Animated World - Base.esp`. Everything downstream is dead without it.
- **Any form `false`** — that esp is present but out of date or edited.
- **`driver live` missing, with an early-exit line instead** — the line names the
  gate: player 3D, `currentProcess`/`middleHigh`, or forms.
- **`driver live` missing AND no early-exit line** — the hook is not running at
  all. That is the dead-duplicate-owner signature; check reachability of the
  owner on their runtime before anything else.

## 4. Animation complaints

The chain is four lines. Whichever is last is where it stopped.

```
[aw] HandlePlayerItem hook reached: item=true loaded3D=true forms=true
[aw] item-added animation requested for the first time
[aw] driver: first arm via <reason>, +<n>ms
[aw] driver: first item-added action, played=true
```

- **No "hook reached"** — the hook never fires. Suspect the site, not the logic.
- **"hook reached" but no "requested"** — one of the guards on that line is
  false; the line prints all three.
- **"requested" but no "arm"** — the driver never armed. On a runtime with clip
  timing it is waiting for a `DynamicIdle` it never sees; without it, waiting
  for `IdleStop`.
- **`played=true` but nothing visible** — fired too early and the running idle
  swallowed it, or fired so early the engine replayed the previous animation.
  This is the tuning knob, not a bug: see below.
- **`played=false`** — the engine refused the action. Check the dummy reference
  and the action form, not the addresses.

## 5. Timing complaints

"The animation plays twice", "the old animation replays", "it cuts off" — all
the same knob. Have them put a millisecond value in:

```
Documents\My Games\Fallout4\F4SE\AnimatedWorld.pickupdelay
```

Read at startup, echoed to the log, accepted 50..10000. It sets the pause after
`IdleStop` on runtimes without clip timing (AE, NG). Raise it if the previous
animation replays. The arm line always says which number is in effect.

On OG the clip is timed directly and this file does nothing — an OG timing
complaint is a different problem, and `[aw] DynamicIdle measured:` reports what
the game told us.

## 6. Two markers worth asking for

Empty files next to the log:

- `AnimatedWorld.tracehooks` — verbose per-step state machine logging. Ask for
  this when the four-line chain above completes but the animation still looks
  wrong.
- `AnimatedWorld.findcallsites` — dumps each hook site's decoded branch and the
  callee's Runtime Database id, before any hook is installed. Ask for this on a
  runtime we have never seen; it is what identified every AE id in this port.

## 7. Known gaps to mention rather than investigate

- Container and console `additem` have never been confirmed on any runtime.
- NG is untested end to end.
- `g_itemFromGround` is never reset after use, so it stays true after any world
  activation. Pre-existing, deliberately kept.
- The Havok `AnimGraphLayout` offsets are verified on OG only, so NG and AE use
  the event path instead of clip timing.

## 8. When it is an address problem

Three failures in this port came from trusting an Address Library id alone: an
id naming a dead duplicate, an id naming an unrelated function, and an id
naming the right function with a different argument order. The techniques that
caught them are written up in `dead-duplicate-owner.md`,
`ae-callsite-corrections.md` and `playidleaction.md` — reachability scanning,
neighbour matching, and reading the callee's prologue. Start there rather than
from the id table.
