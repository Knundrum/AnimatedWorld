# AnimatedWorld (CommonLibF4RD)

Port of AnimatedWorld from the in-tree CommonLibF4 fork to
[CommonLibF4RD](https://github.com/Zzyxz/CommonLibF4RD).

Behaviour on Fallout 4 1.10.163 (OG) is unchanged apart from the bug fixes listed
below. **AE 1.11.240 is supported**: every address resolves, all six hook sites
are located by `REL::AUTO_CALLSITE`, and activate, equip, consume, add-item and
ground pickup are confirmed in game.

NG 1.10.984 is untested. Two of its interior offsets are deliberately left unset
so those sites fail closed there rather than writing a branch into an unverified
address — see **Runtime support**.

## Building

> **Visual Studio 2022 only.** CommonLibF4RD needs the **v143** toolset.
> Visual Studio 2019 ships v142 and cannot build it — you get
> `MSB8020: The build tools for v143 ... cannot be found`.
> If VS2019 offers to **"Retarget solution"**, say no: that downgrades the
> generated project to v142, and CMake overwrites it on the next configure
> anyway.

The simplest route, which picks VS2022 for you even if VS2019 is also installed:

```
git clone https://github.com/Zzyxz/CommonLibF4RD.git external/CommonLibF4RD
build.bat            :: Release
build.bat Debug      :: Debug
```

Or by hand, from a **VS2022** x64 Native Tools prompt:

```
cmake --preset vs2022-windows-vcpkg
cmake --build --preset vs2022-release
```

To work in the IDE, open `build\vs2022\AnimatedWorld.sln` **in Visual Studio
2022**, not 2019. A `ninja-windows-vcpkg` preset is also available for faster
iteration; it needs a VS2022 developer prompt so that `cl.exe` is v143.

`VCPKG_ROOT` must be set. To use a CommonLibF4RD checkout that lives somewhere
else, configure with `-DCOMMONLIBF4RD_PATH=<path>`.

`CMakeSettings.json` is kept for the "Open Folder" workflow — but Visual Studio
prefers it over `CMakePresets.json`, and it builds with whichever Visual Studio
opened the folder. Open the folder in VS2022, or delete that file and use the
presets.

### Do not put `/W4 /WX` in `CMAKE_CXX_FLAGS`

Those are global, so they also apply to CommonLibF4RD, which is added with
`add_subdirectory`. Its sources do not build warning-clean at `/W4` — its
CMakeLists suppresses a list that does not include C4100, and
`Actor.h` has an unreferenced `a_mount` parameter. The library then fails to
produce `CommonLibF4.lib` and this project dies much later with the misleading

```
LINK : fatal error LNK1181: cannot open input file 'CommonLibF4\Release\CommonLibF4.lib'
```

The presets therefore pass only `/EHsc /MP`; `/W4 /WX` and the suppression list
live in `target_compile_options` on the AnimatedWorld target. `CMakeLists.txt`
additionally silences C4100 on the `CommonLibF4` target so a stray global flag
cannot resurrect this.

End users additionally need the CommonLibF4RD Runtime Database at
`Data/F4SE/Plugins/f4rd-runtime.bin`. It ships separately from the library.

## Runtime support

CommonLibF4RD makes *offsets* runtime-independent, not *ids*: OG and AE use
separate numeric id spaces and the only automatic bridge runs AE → OG. An OG id
left in the AE slot does not fail — it resolves to whatever unrelated function
owns that number on AE — so an id that is not known for a runtime is left as
`UNKNOWN_ID` and the feature that needs it is simply not installed.

Everything address-related lives in **`src/Addresses.h`**. Nothing else needs
editing to add a runtime.

| What | OG id | AE id | AE callee id |
| --- | --- | --- | --- |
| `PlayAction` (`PlayIdleAction`) | 1451490 | 2251634 | — |
| `ApplyMaterialSwap` | 708895 | 2189271 | — |
| `IsActivationBlocked` | 407609 | 2201146 | — |
| `WornHasKeyword` | 900857 | 2200995 | — |
| `PlayPipboyOpenAnim` | 663900 | 2225444 | — |
| `RunActorUpdates` | 556439 | 2227611 | 2228929 |
| `AddAcquiredEvent` | 1401485 | 2221653 | 2232930 |
| `ActivateRef` | 785533 | 2233039 | 2201147 |
| `HandlePlayerItem` | 78185 | 2200949 | 2222424 |
| `UseObject` | 988029 | 2231392 | 2231415 |
| `SetInputDeviceLightState` | 520007 | 2233201 | 2268396 |

Every hook site carries a `callsiteTarget` — the id of the function *called* at
that site — so CommonLibF4RD locates the call itself via `REL::AUTO_CALLSITE` and
survives a shifted function body. The hardcoded interior offsets are fallbacks on
both runtimes, not load-bearing. A fixed offset is also checked before use: if
the byte there is not the expected `E8`/`E9` branch, that one hook is skipped and
says so in the log, rather than writing five bytes into the middle of an
instruction.

Three of those AE ids were wrong when first supplied from an id table, in three
different ways, and none of the three failures is visible from an id lookup. If
you are porting something similar, `docs/` is where the interesting part of this
project is.

### Structure layouts are a separate problem

Runtime-aware addresses do not make class layouts portable. `Game::ReadCurrentClip`
walks Havok structures by raw byte offset (`AnimGraphLayout` in `src/Game.cpp`),
verified only on OG. On NG/AE it is marked unverified and disabled; there the
item-added animation instead waits for the animation graph's `IdleStop` event and
adds a tail, which depends on no struct layout at all. Once the offsets are
confirmed on a real NG/AE binary, drop the `Unverified()` call.

The wait after `IdleStop` is tunable without a rebuild: put a number of
milliseconds on the first line of
`Documents\My Games\Fallout4\F4SE\AnimatedWorld.pickupdelay`.

End users additionally need the CommonLibF4RD Runtime Database at
`Data/F4SE/Plugins/f4rd-runtime.bin`. It ships separately from the library.

## Notes worth reading before porting your own mod

`docs/` holds what this port actually cost, written up so it does not have to be
rediscovered:

| File | What it covers |
| --- | --- |
| `triage.md` | Reading a user's log: symptom to cause, and which marker files to ask for. |
| `playidleaction.md` | Identifying a function by the error strings only it references, and an argument order that *changed* between OG and AE. |
| `dead-duplicate-owner.md` | AE ships the per-frame update driver twice; the Address Library id names the copy nothing calls. How to check reachability. |
| `ae-callsite-corrections.md` | Finding the right callsite by the instructions either side of it when the id is for a different function. |
| `blind-pickup-wait.md` | Timing an animation you cannot measure, and why an RTTI base-offset diff disproved a layout theory. |
| `useobject.md` | A signature that was wrong on every runtime for years and worked anyway. |

Two diagnostic markers, empty files next to the F4SE log, are useful on any
runtime this has not seen: `AnimatedWorld.findcallsites` dumps each hook site's
decoded branch and the callee's Runtime Database id, and `AnimatedWorld.tracehooks`
turns on verbose state-machine logging.

## What changed in the port

**Plugin entry point.** `F4SEPlugin_Query` and the `RUNTIME_1_10_162` version
check are gone, replaced by a `constinit F4SEPlugin_Version` declaring
`kAddressIndependence_Signatures`. Per CommonLibF4RD's guidance an unknown patch
number is no longer a reason to refuse to load — whether the plugin can work is
decided by whether its addresses resolve. `AllocTrampoline` moved from `Query`
into `Load`.

**Addresses.** All eleven ids and six interior offsets moved out of the middle of
`hooks.cpp` into `src/Addresses.h`. Nothing uses `REL::Relocation`'s constructor
for an optional address, because that constructor calls `stl::report_and_fail`
and kills the process; resolution goes through `IDDatabase::resolve`, which
reports failure instead.

**APIs the RD library does not have.** The old in-tree CommonLibF4 was a fork
carrying `TESObjectREFR::WornHasKeyword`, `PipboyManager::PlayPipboyOpenAnim` and
a full `BSAnimationGraphManager` definition. Upstream CommonLibF4RD has none of
them, so they are reimplemented locally in `src/Game.{h,cpp}` and the library is
left unforked. `BSAnimationGraphEvent::tag` is `animEvent` upstream (same offset,
same size).

**Dropped.** `src/detourxs/` — nothing referenced it, and `LDE64x64.lib` was never
linked, so it was dead weight. The unused `playerRef` global and the declared-but-
never-defined `InstallVTableHook()` are gone too.

### Bug fixes

*Crash safety*

- `HookedAddEvent`'s null guard called the original and then **fell through and
  used the null pointers anyway** — a missing `return`.
- The per-frame update called `PlayAction` with `AW_ItemAddedAction` without
  checking it resolved; with the esp missing or not yet loaded that is a null
  action pointer.
- `RE::UI::GetSingleton()` and `RE::TESDataHandler::GetSingleton()` were
  dereferenced unchecked.
- `GetClipInfo` walked the active-generator array until it found a null with no
  bound, and null-checked only some of the pointers on the way down. Every step
  is now checked and the walk is bounded.
- The player vtable patch now verifies it read a non-null original before
  writing, and the `try`/`catch` around a call that cannot throw is gone.

*Behaviour*

- **Stale material swap.** `g_currentSwap` was set on the pickup path and never
  cleared, so a book's swap could be applied to an unrelated item that arrived
  later through a different path. The swap is now remembered together with the
  item it came from and is only applied if the animation target still holds that
  item — which also makes it independent of the order the two hooks fire in.
- **Flashlight hook fired with no player 3D**, including during load. It now
  requires a fully loaded player.
- **`IdleStopFix` latched forever** if the `IdleStop` event never arrived, so an
  unrelated later animation consumed it. It expires after 2s.
- **A pending animation polled forever** if its clip never became `DynamicIdle`.
  It now gives up after 3s.
- **Pip-Boy could be left closed.** The equip animation tears the Pip-Boy down and
  relies on `PlayPipboyOpenAnim` to restore it; if that address is unavailable the
  animation is now skipped rather than played with no way back.

*Unchanged on purpose*

- The dummy `TESObjectREFR`s are still created through the form factory and never
  registered or freed. That is how the mod addresses arbitrary items with one
  `BGSAction`, and changing it is a redesign, not a fix.
- The vtable patch is never reverted. Harmless for a game process.
