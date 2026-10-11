#pragma once

// ---------------------------------------------------------------------------
// Every address this plugin depends on lives in this file.
//
// CommonLibF4RD's REL::ID takes ids in the order (OG, NG, AE):
//
//     REL::ID{ ae }            one portable AE id, bridged to OG/NG if possible
//     REL::ID{ og, ae }        NG shares the AE id
//     REL::ID{ og, ng, ae }    all three explicit
//
// OG and AE use INDEPENDENT numeric id spaces.  There is no OG -> AE bridge:
// IDDatabase::resolve() consults ae_id() and nothing else when the game is AE.
// Putting an OG number in the AE slot therefore does not fail - it resolves to
// whatever unrelated function owns that number on AE, and the hook lands in the
// middle of it.  So an id that is not known for a runtime family is left as
// UNKNOWN_ID and the owning feature is simply not installed on that runtime.
//
// TO ADD AE/NG SUPPORT: replace an UNKNOWN_ID with the real id and, for hook
// sites, the matching interior offset.  Nothing else needs to change; the
// capability report at startup will pick it up.  Better still, give the site a
// `callsiteTarget` (the id of the function being called at that site) and
// CommonLibF4RD will discover the offset itself via REL::AUTO_CALLSITE.
// ---------------------------------------------------------------------------

namespace AW::Addresses
{
	// An id that has not been determined for a runtime family yet.
	inline constexpr std::uint64_t UNKNOWN_ID = REL::ID::INVALID_ID;

	// An interior hook offset that has not been determined yet.
	inline constexpr std::ptrdiff_t UNKNOWN_OFFSET = (std::numeric_limits<std::ptrdiff_t>::min)();

	// -----------------------------------------------------------------------
	// Functions the plugin calls directly.
	// -----------------------------------------------------------------------

	// bool PlayIdleAction(...) - the Papyrus native behind Actor.PlayIdleAction.
	// Identified on BOTH binaries by the two error strings it is the only
	// function to reference: "Cannot run a None action." and "Cannot run an
	// action on a None actor".
	//
	//   OG 1.10.163  id 1451490  rva 0x13865c0 .. 0x138674c
	//   AE 1.11.240  id 2251634  rva 0x10fa960 .. 0x10faabd
	//
	// The OG id is the one this mod has always used, so the pairing is anchored
	// at both ends.  Actor::PerformAction (OG 1057231 / AE 2231177) is a
	// DIFFERENT function - OG 1057231 is rva 0xe04fb0, nowhere near 0x13865c0 -
	// and must not be substituted here.
	//
	// The parameter order differs between the two families; see Game.cpp.
	inline constexpr REL::ID PlayAction{ 1451490, 2251634, 2251634 };

	// void ApplySwap(NiAVObject*, BGSMaterialSwap const*, float, float, void*)
	// NG/AE from AnimatedWorldFramework PR #1.  Same OG anchor (708895), and its
	// notes put the OG implementation at rva 0x531B0 - which matches the rva our
	// own OG session reported for this id.
	inline constexpr REL::ID ApplyMaterialSwap{ 708895, 2189271, 2189271 };

	// bool TESObjectREFR::IsActivationBlocked(TESObjectREFR*)
	// NG/AE from AnimatedWorldFramework PR #1.  Independently corroborates the
	// prediction made from rva adjacency: IsActivationBlocked sits immediately
	// before ActivateRef (2201147) on OG, and gets the adjacent id 2201146.
	inline constexpr REL::ID IsActivationBlocked{ 407609, 2201146, 2201146 };

	// bool TESObjectREFR::WornHasKeyword(TESObjectREFR*, BGSKeyword*)
	// Present in the old in-tree CommonLibF4 fork, absent from CommonLibF4RD.
	//
	// AE id from commonlib_NonVR PR #88 (2026-09-17), which names AE 2200995 as
	// TESObjectREFR::WornHasKeyword at RVA 0x507940 on 1.11.240, from Ghidra and
	// Address Library evidence.  STATIC PROOF ONLY - that PR states explicitly
	// that no live runtime result is claimed, and its own pre-merge gate (a probe
	// on real 1.10.163 / 1.10.984 / 1.11.240 sessions) has not been run.  Verify
	// the RVA against a real AE session before trusting it.  Inert on OG.
	inline constexpr REL::ID WornHasKeyword{ 900857, 2200995, 2200995 };

	// void PipboyManager::PlayPipboyOpenAnim(PipboyManager*, const BSFixedString&)
	// Present in the old in-tree CommonLibF4 fork, absent from CommonLibF4RD.
	inline constexpr REL::ID PlayPipboyOpenAnim{ 663900, 2225444, 2225444 };

	// -----------------------------------------------------------------------
	// Hook sites.
	//
	// `owner` is the function that CONTAINS the call being replaced; the
	// offsets are relative to the start of that function, and the site's NAME
	// describes the CALLEE rather than the owner.
	//
	// Every site now carries a callsiteTarget, so the interior offsets are only
	// a fallback: REL::AUTO_CALLSITE locates the call by resolving owner and
	// callee and finding the branch between them.  On OG that is self-checking,
	// because the offset it discovers should equal the ogOffset below - a
	// mismatch means the callee id is wrong.
	// -----------------------------------------------------------------------

	enum class Site : std::size_t
	{
		kRunActorUpdates,
		kAddAcquiredEvent,
		kActivateRef,
		kHandlePlayerItem,
		kUseObject,
		kSetInputDeviceLightState,

		kTotal
	};

	// A raw rva for the owning function, per runtime family, for the case
	// where the Address Library id resolves to the WRONG function.  That is
	// not hypothetical: on AE 1.11.240 the id for the per-frame actor update
	// driver points at an unreferenced duplicate of it, so a hook installed
	// there is never executed.
	//
	// A raw rva is pinned to one exact build, so it is only honoured when the
	// branch it selects actually targets this site's callsiteTarget.  If it
	// does not, the site fails closed rather than writing into whatever
	// happens to live at that address on a different build.
	struct OwnerRva
	{
		std::uintptr_t og{ 0 };
		std::uintptr_t ng{ 0 };
		std::uintptr_t ae{ 0 };
	};

	struct HookSite
	{
		std::string_view name;

		// Function containing the call to replace.
		REL::ID owner;

		// Interior offsets, one per runtime family.
		std::ptrdiff_t ogOffset;
		std::ptrdiff_t ngOffset;
		std::ptrdiff_t aeOffset;

		// Optional: the id of the function called at this site.  When set and
		// resolvable, REL::resolve_callsites finds the offset itself and the
		// fixed offsets above are only a fallback.  This is the update-resilient
		// form recommended by CommonLibF4RD and is worth filling in even for OG.
		REL::ID callsiteTarget{};

		// How the site branches to that target.  Confirmed per site by the
		// AnimatedWorld.findcallsites dump: five are E8 calls, the Pip-Boy light
		// site is an E9 jmp (a tail call).  resolve_callsites will not match a
		// jmp while looking for a call, so this must be right or the automatic
		// lookup silently falls back to the fixed offset.
		REL::AutoCallsiteBranch branch{ REL::AutoCallsiteBranch::kCall };

		// Set only for a family whose owner id is known to be wrong.  Validated
		// against callsiteTarget before use; see OwnerRva above.
		OwnerRva ownerRva{};

		// True when this site must exist for the plugin to be worth loading.
		bool required{ false };
	};

	inline constexpr std::array<HookSite, static_cast<std::size_t>(Site::kTotal)> kHookSites{ {
		// Per-frame actor update driver.  Everything time-based hangs off this
		// one, so without it the plugin does nothing at all.
		// Owner 556439 is unnamed in CommonLibF4RD; the site's callee has the
		// signature void(void*).  The site NAME here describes the callee, not
		// the owner - true of every entry in this table.
		//
		// AE 1.11.240 ships this function TWICE.  Id 2227608 resolves to rva
		// 0xbd65a0, which has no callers and no pointer references anywhere in
		// the image - dead code, a cold duplicate.  The live copy is 0xbd6850,
		// reached through a callback pointer at 0x2f30fc0; it is the same
		// function with the null guards optimised out, which shifts the call
		// to the callee from +0xf0 to +0xfc.
		//
		// So on AE the hook installed cleanly at a correct-looking callsite in
		// a function the game never runs.  Everything that fires from its own
		// hook kept working; only the per-frame state machine went silent.
		// A temporary rva override pointed at the live copy long enough for the
		// log to report which id owns it: 2227611, one past the dead 2227608.
		// The override is gone; the id below is the real fix.
		//
		// NG keeps 2227611 as well, on the same reasoning as everywhere else in
		// this file - NG and AE share an id space - but no NG binary has been
		// checked, so if NG ever goes quiet the way AE did, run the same
		// reachability scan on its copy first.
		HookSite{
			.name = "RunActorUpdates"sv,
			.owner = REL::ID{ 556439, 2227611, 2227611 },
			.ogOffset = 0xF0,
			.ngOffset = 0xFC,
			.aeOffset = 0xFC,
			// Callee is unnamed in CommonLibF4RD; OG id measured from a live
			// session, AE id confirmed by the live callsite dump.
			.callsiteTarget = REL::ID{ 1318162, 2228929, 2228929 },
			.required = true },

		// PlayerCharacter item-acquired event.
		// Callee on OG is id 876119 (rva 0xe9f220), unnamed in CommonLibF4RD.
		HookSite{
			.name = "AddAcquiredEvent"sv,
			.owner = REL::ID{ 1401485, 2221653, 2221653 },
			.ogOffset = 0x2D6,

			.ngOffset = 0x354,
			.aeOffset = 0x354,
			.callsiteTarget = REL::ID{ 876119, 2232930, 2232930 } },

		// Owner 785533 is unnamed in CommonLibF4RD.  The findcallsites dump
		// confirms the callee at +0x38A is OG id 753531, which CommonLibF4RD
		// names TESObjectREFR::ActivateRef with AE id 2201147 - so this site has
		// a complete callsite target and no longer needs an interior offset on
		// any runtime.  The ogOffset stays as the fallback.
		//
		// This is the only site where automatic resolution is live.  If activate
		// animations break, delete the callsiteTarget line: the log says which
		// path was taken ("callsite discovered automatically" vs the warning).
		HookSite{
			.name = "ActivateRef"sv,
			.owner = REL::ID{ 785533, 2233039, 2233039 },
			.ogOffset = 0x38A,
			// step 2: callsiteTarget REL::ID{ 753531, 2201147, 2201147 } -
			// held back because setting it enables AUTO_CALLSITE on OG too.

			.ngOffset = 0x31D,
			.aeOffset = 0x31D,
			.callsiteTarget = REL::ID{ 753531, 2201147, 2201147 } },

		// Owner is TESObjectREFR::AddInventoryItem, REL::ID(78185, 2200949) in
		// CommonLibF4RD.  We replace a call inside it at +0xA40 on OG, where
		// the callee is id 357079 (rva 0xae1160) and is called exactly once.
		//
		// The AE numbers taken from AnimatedWorldFramework PR #1 were wrong on
		// both counts.  Callee 2194003 (rva 0x3477c0) is a DIFFERENT function -
		// AE calls it three times inside this owner, at +0x5cb, +0x825 and
		// +0xa4a, and always as (lea, lea, ptr), not the (item, extras, count)
		// this hook expects.  The fixed offset 0xa4a landed on the third of
		// those, so every AE build so far replaced an unrelated call with a
		// mismatched signature.
		//
		// The real site is at +0xB17 -> rva 0xa36880, located by the code either
		// side of it rather than by id: on both runtimes the call sits between a
		// comparison against the PlayerCharacter singleton and a
		// `cmp byte [item+0x1a], 0x2c` form-type test, and takes the same
		// (rcx = item, rdx = extras, r8d = count).  That pairing is unique in
		// both images.
		//
		// The AE callsite dump then named the real callee: rva 0xa36880 is id
		// 2222424, and it is called exactly once inside this owner - so
		// AUTO_CALLSITE resolves cleanly now and the ambiguity warning is gone.
		// NG's offset came from the same bad source as the old AE numbers, so
		// it stays cleared - fail closed rather than write into the wrong call.
		HookSite{
			.name = "HandlePlayerItem"sv,
			.owner = REL::ID{ 78185, 2200949, 2200949 },
			.ogOffset = 0xA40,

			.ngOffset = UNKNOWN_OFFSET,
			.aeOffset = 0xB17,
			.callsiteTarget = REL::ID{ 357079, 2222424, 2222424 } },

		// Owner is ActorEquipManager::EquipObject, REL::ID(988029, 2231392) in
		// CommonLibF4RD - a thin wrapper that hands four arguments
		// (this, Actor*, BGSObjectInstance*, params*) to an inner worker.
		//
		// OG (0xe1bcd0) calls TWO different workers: a fast path at +0x15A ->
		// 0xe1d100 (id 301794), and, if that returns false, a locked slow path
		// at +0x196 -> 0xe1eb50.  This hook has always taken the fast one.
		//
		// AE (0xce5e20) merged them.  There is exactly one four-argument worker
		// call in the whole function, at +0x283 -> rva 0xce7fb0, and every path
		// converges on it - which is why REL::AUTO_CALLSITE reported
		// callsite_not_found for id 2231408 on 1.11.240: no call to that id
		// exists here.  The offset was measured by disassembly, and the live AE
		// callsite dump then named the callee: rva 0xce7fb0 is id 2231415, not
		// 2231408.  With the real id in place AUTO_CALLSITE takes over and the
		// offset goes back to being a fallback.
		//
		// NG is left unset on purpose.  Its offset was not measured and a wrong
		// interior offset writes a branch into the middle of an instruction.
		HookSite{
			.name = "UseObject"sv,
			.owner = REL::ID{ 988029, 2231392, 2231392 },
			.ogOffset = 0x15A,

			.ngOffset = UNKNOWN_OFFSET,
			.aeOffset = 0x283,
			.callsiteTarget = REL::ID{ 301794, 2231415, 2231415 } },

		// Owner is PlayerCharacter::TogglePipBoyLight, REL::ID(520007, 2233201)
		// in CommonLibF4RD.  The findcallsites dump shows the site is an E9 JMP
		// (a tail call), not a call - hence the kJump branch kind below.
		// Callee on OG is id 157452 (rva 0x1b2c080), unnamed in CommonLibF4RD.
		HookSite{
			.name = "SetInputDeviceLightState"sv,
			.owner = REL::ID{ 520007, 2233201, 2233201 },
			.ogOffset = 0x5B,

			.ngOffset = 0x5B,
			.aeOffset = 0x5B,
			
			.callsiteTarget = REL::ID{ 157452, 2268396, 2268396 },
			.branch = REL::AutoCallsiteBranch::kJump },
	} };

	[[nodiscard]] constexpr const HookSite& GetSite(Site a_site) noexcept
	{
		return kHookSites[static_cast<std::size_t>(a_site)];
	}

	// -----------------------------------------------------------------------
	// Resolution.  None of these ever call stl::report_and_fail: an id that
	// cannot be resolved yields std::nullopt and the caller declines the
	// feature.  REL::Relocation's constructor is deliberately not used for
	// anything optional, because it terminates the process on failure.
	// -----------------------------------------------------------------------

	// True when a_id carries an id for the family the game is actually running.
	[[nodiscard]] bool HasIDForRuntime(const REL::ID& a_id) noexcept;

	// Absolute address of a function, or nullopt with a logged reason.
	[[nodiscard]] std::optional<std::uintptr_t> ResolveFunction(
		std::string_view a_name,
		const REL::ID& a_id);

	// Absolute address of a hook site, or nullopt with a logged reason.
	[[nodiscard]] std::optional<std::uintptr_t> ResolveSite(Site a_site);

	// One block in the log naming every id, whether it resolved, and why not.
	void LogCapabilityReport();

	[[nodiscard]] std::string_view RuntimeFamilyName() noexcept;
}
