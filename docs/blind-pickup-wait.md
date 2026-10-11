# Ground pickup on a runtime that cannot time the animation

## What the log showed

```
21.600  HandlePlayerItem hook reached
21.600  item-added animation requested for the first time
21.708  driver: first arm via itemFromGround, +100ms
21.816  driver: first item-added action, played=true
```

`played=true` and no animation. The action was delivered 216 ms after the
pickup started, straight into the still-running pickup idle, which swallowed
it. This is the same failure the arming comments have warned about since the
port began — it just took a runtime without clip timing to expose it.

## Why it only happened on AE

Two conditions can arm the deferred animation, and they were never symmetric:

- `g_itemFromGround` arms at `GROUND_PICKUP_DELAY` (100 ms).
- A playing `DynamicIdle` arms at `remaining + DYNAMIC_IDLE_TAIL`.

On OG the 100 ms is only ever a **floor**. The clip walk sees `DynamicIdle`
a frame or two later and replaces the short delay with the real remaining time,
so the action lands after the pickup finishes.

On AE the Havok offsets are unverified, so `ReadCurrentClip` is switched off and
`DynamicIdle` is never seen. Nothing ever refines the 100 ms. The old fallback
branch was guarded by `if (!arm && ...)`, so once `g_itemFromGround` had armed,
the blind wait never applied at all.

Every other path worked because activate, equip and consume fire immediately
from their own hooks and never touch this state machine.

## Fix

When the clip walk is unavailable the blind wait now applies unconditionally,
raising any shorter delay rather than being skipped:

```cpp
if (!Game::CanReadClipInfo()) {
    if (!arm || delay < g_clipUnreadableFallback) {
        delay = g_clipUnreadableFallback;
        reason = "clip walk unavailable";
    }
    arm = true;
}
```

The default rose from 700 ms to **1100 ms**, which is a considered guess rather
than a measurement — 700 ms was never exercised, because the `!arm` guard meant
the ground-pickup path never reached it.

## Tuning it without a rebuild

Put a number of milliseconds on the first line of:

```
Documents\My Games\Fallout4\F4SE\AnimatedWorld.pickupdelay
```

Read once at startup, reported in the log, accepted between 50 and 10000.
Values outside that range are ignored — a zero or negative wait would fire the
action into the running idle, which is the bug this knob exists to avoid.

## Getting the real number

The driver now reports the first `DynamicIdle` it can time:

```
[aw] DynamicIdle measured: duration=N.NNNs at t=N.NNNs - a blind wait needs to cover this
```

That line only appears where the clip walk works, so **run it once on OG** and
read the duration. That is the value AE is waiting blind for, and it should
become the default here.

## The real fix, eventually

Verify the eleven `AnimGraphLayout` offsets in `Game.cpp` against an AE binary
and drop the `Unverified()` call for that family. The clip walk then works on
AE and the blind wait stops mattering, exactly as on OG.

## Better: wait for the event, not for the clock

A fixed wait is wrong in principle — pickup idles vary in length, and the
design has always been *detect that the animation finished, then add an
offset*. On OG that is what the clip walk does. The measured OG numbers:

```
[aw] DynamicIdle measured: duration=0.770s at t=0.110s
[aw] driver: first arm via DynamicIdle, +859ms
```

0.660 s remaining plus the 200 ms tail. A 1100 ms fixed wait happens to clear a
0.770 s idle, but it would cut a longer one short and idles for anything else.

The animation graph event sink is already hooked for the IdleStop fix, and
graph events are **strings** — no struct offsets, so they work on every runtime
family, unlike the Havok clip walk. That makes them the right completion
signal where the clip cannot be timed.

So on a runtime without clip timing, the wait now ends on the `IdleStop` event
while an item-added animation is pending, plus `DYNAMIC_IDLE_TAIL`:

```
if (g_idleEndSeen) {
    delay  = DYNAMIC_IDLE_TAIL;
    reason = "idle stopped";
} else if (!arm || delay < g_clipUnreadableFallback) {
    delay  = g_clipUnreadableFallback;
    reason = "clip walk unavailable";
}
```

Same shape as the timed path — finish, then offset — and it tracks a pickup of
any length. The fixed wait survives only as the backstop for when the event
never arrives, which is what a 1100 ms default is actually suited to.

OG is untouched: this whole branch is behind `!Game::CanReadClipInfo()`, so the
proven clip-timing path still runs there.

### Confirming the event

`IdleStop` is the reasonable bet, not a measurement. While an animation is
pending, each distinct graph event name is now reported once:

```
[aw] graph event while pending: '<name>'
```

Read that list from an AE run. The arming line then says which path won —
`arm via idle stopped, +200ms` (event) or `arm via clip walk unavailable,
+1100ms` (backstop). If the backstop is winning, the completion event for a
pickup is one of the other names in that list, and it goes in beside
`EVENT_IDLE_STOP`.

## The event never arrived, and that is its own finding

First AE run with the event path:

```
[aw] driver: first arm via clip walk unavailable, +1100ms
```

The backstop won — and there were **no** `graph event while pending` lines at
all. Not "the wrong event fired": no event was seen.

Every branch of `HookedProcessGraphEvent` sits behind one identity check:

```cpp
static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(player) == a_this
```

`TESObjectREFR` inherits that sink at **+0x038** in CommonLibF4RD, which is a
compile-time base offset — struct layout again, the one thing a runtime address
database cannot make portable. `InstallGraphEventHook` reads the vtable pointer
from `player + 0x38` and patches slot 1, so if that offset is wrong on a
runtime, the patch lands on a vtable nothing dispatches through and the sink
goes quiet without any error.

This matters beyond the pickup wait: **the IdleStop fix and the Pip-Boy reopen
both hang off this same sink**, so if it is dead on AE, those are silently dead
there too, and have been since the port.

The hook now reports its first three entries with both pointers and the match
result, plus the base offset at install time. Three outcomes:

- **Entries logged, `match=true`** — the sink works; `IdleStop` is simply not
  the event that ends a pickup. Pick the right name from the reported list.
- **Entries logged, `match=false`** — the sink fires but never for the player.
  The base offset is wrong for this runtime.
- **No entries at all** — the patched vtable is not on the dispatch path.

Run the same build on OG as well. If OG logs entries and AE does not, the fault
is AE layout. If neither does, this sink has never worked on any runtime and
two existing features have been quietly broken all along.

## Correction: the base offset is not the problem

The layout theory above is wrong, and the binaries say so directly.

MSVC emits one RTTI Complete Object Locator per vtable-bearing base subobject,
each carrying that base's offset within the complete object. Collecting every
COL whose type descriptor is `PlayerCharacter`, and the same for
`TESObjectREFR`, gives the full list of base offsets — and OG and AE are
byte-identical:

```
PlayerCharacter  0x0 0x20 0x30 0x38 0x40 0x48 0x50 0x58 0x110 0x128 0x138
                 0x140 0x148 0x150 0x158 0x160 0x168 0x490 0x498 0x4a0
                 0x4a8 0x4b0 0x4b8 0x4c0
TESObjectREFR    0x0 0x20 0x30 0x38 0x40 0x48 0x50 0x58
```

`BSTEventSink<BSAnimationGraphEvent>` sits at `+0x38` on both. So
`static_cast<BSTEventSink<BSAnimationGraphEvent>*>(player)` produces the same
pointer on AE as on OG, and `InstallGraphEventHook` patches the same slot of the
same vtable. Whatever silences the sink on AE, it is not this.

Worth keeping as a technique: **RTTI COL offsets are a free, exact layout diff
between two builds**, no debug symbols required. Any class whose base list is
in doubt can be checked this way in seconds.

## What the runs actually showed

OG, during a ground pickup:

```
[aw] graph event while pending: 'initiateStart'
[aw] graph event while pending: 'SoundStop'
[aw] DynamicIdle measured: duration=0.770s at t=0.110s
[aw] driver: first arm via DynamicIdle, +859ms
```

Two things follow. The sink works on OG and its identity check passes — so the
Pip-Boy reopen and IdleStop fix are alive there. And `IdleStop` is **not**
among the events, so it was the wrong completion signal to bet on.

The window was also too narrow to tell: `g_animationPending` ends the moment
the driver arms, about 100 ms in, so the log covered the first tenth of a second
of a 0.77 s animation. It now stays open through `g_animationSoon`, to the
moment the item-added action fires.

AE logged nothing at all in the same window — but that run predates the
unconditional entry logging, so it still cannot distinguish "sink never
entered" from "entered but never matched the player".

## Resolved: the sink was fine, the arming order was not

Both runtimes, same build, entry logging on:

```
OG  graph event sink entered: this=0x1a772e79b38 playerSink=0x1a772e79b38 match=true
AE  graph event sink entered: this=0x2b46f5a8ee8 playerSink=0x2b46f5a8ee8 match=true
```

The sink works on AE and always did. The earlier silence was an artefact of the
observation window: it was open only while `g_animationPending`, which ended
~108 ms in when the driver armed, and on AE nothing happened to be dispatched
in that first tenth of a second. Nothing was broken; the instrument was too
short. The IdleStop fix and Pip-Boy reopen are alive on both runtimes.

`IdleStop` is also confirmed as the completion event, on both:

```
OG  47.001 request → 47.722 IdleStop   (+721ms)   DynamicIdle predicted +860ms
AE  42.792 request → 43.267 IdleStop   (+475ms)
```

On OG the event lands 140 ms before the timed path's own estimate, so the two
mechanisms agree to within a frame or two of tail.

### The actual bug

AE fired at the deadline despite the event arriving:

```
42.792  item-added animation requested
42.900  arm via clip walk unavailable, +1100ms
43.267  IdleStop          <- 367ms too late to matter
44.001  item-added action fired
```

The driver arms **once**, on the first tick past the settle delay, and then
only waits out the delay it chose. `g_idleEndSeen` was set correctly at 43.267,
but by then `g_animationPending` was already false and the arming block was
never re-evaluated. A completion signal that arrives after the decision is not
a completion signal.

### The fix

Where the clip cannot be timed, stay pending until the event arrives, then add
the same tail the timed path uses:

```cpp
if (g_idleEndSeen) {
    arm = true;  delay = DYNAMIC_IDLE_TAIL;  reason = "idle stopped";
} else if (now >= g_blindDeadline) {
    arm = true;  delay = DYNAMIC_IDLE_TAIL;  reason = "no IdleStop before the deadline";
} else {
    arm = false;                              // keep waiting
}
```

The fixed wait is now a **deadline for giving up on the event**, not the delay
itself — which is what it should have been from the start. Default raised to
1500 ms, since a deadline wants margin where a delay wanted accuracy, and
observed arrivals are 475–720 ms. `AnimatedWorld.pickupdelay` still overrides
it, with the same meaning.

Expect `arm via idle stopped, +200ms` on AE now, roughly 675 ms after the
request rather than a flat 1100 ms — and it will track a longer or shorter
pickup instead of assuming one.

### Worth doing later

The event path needs no Havok offsets and is now confirmed on both runtimes, so
it could replace the clip walk everywhere rather than only where the clip walk
is missing. That would remove the last struct-layout dependency in the timing
code. OG is left on the timed path for now because it is proven and there is no
reason to churn it mid-release.

## The pause after IdleStop is its own number

The event path works — `arm via idle stopped, +200ms` — but 200 ms was not
enough on AE: the previous animation replays instead of the new one starting.

200 ms was `DYNAMIC_IDLE_TAIL`, borrowed from the timed path, and the two are
not the same measurement. The timed path knows where the clip actually ends and
only needs to clear it. `IdleStop` fires as the idle *begins* unwinding, with
the graph still settling — in the same millisecond as `AnimObjUnequip` and
`initiateStart`, per the logs — so it needs more room after it.

Split into `IDLE_STOP_TAIL`, default **400 ms**, used only by the event path.
`DYNAMIC_IDLE_TAIL` stays 200 ms and OG is unaffected.

`AnimatedWorld.pickupdelay` now tunes this pause, since it is the number that
decides how the pickup feels. The give-up deadline is derived from it rather
than set separately — it only matters when no `IdleStop` arrives at all, so it
just has to stay clear of the tail.

Raise the value if the old animation still replays; lower it if the pause
between putting the item away and the item-added animation feels slack.
