#pragma once

// ---------------------------------------------------------------------------
// Thin wrappers over engine functions, plus the one API the old in-tree
// CommonLibF4 fork carried that upstream CommonLibF4RD does not
// (PipboyManager::PlayPipboyOpenAnim).
//
// Everything here is optional: if an address could not be resolved on the
// running runtime the wrapper is inert and the matching Can*() query says so,
// rather than the process dying inside REL::Relocation's constructor.
// ---------------------------------------------------------------------------

#include "RE/Bethesda/Actor.h"
#include "RE/Bethesda/PipboyManager.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/TESObjectREFRs.h"

#include <string>

namespace AW::Game
{
	// Resolves every engine address once.  Returns false when something the
	// plugin cannot work without is missing.
	[[nodiscard]] bool Initialize();

	// Calls the PlayIdleAction Papyrus native.  The engine takes a VM pointer
	// and a stack id as well; both are passed as null/0 and the argument order
	// differs per runtime family, which Game.cpp handles.
	[[nodiscard]] bool PlayAction(RE::Actor* a_actor, RE::BGSAction* a_action, RE::TESObjectREFR* a_target);

	[[nodiscard]] bool CanApplyMaterialSwap() noexcept;

	// A material swap matches geometry by MATERIAL PATH, not by node, so the
	// subtree this is given is the whole of its blast radius.  Hand it the
	// narrowest node that holds the thing being recoloured.
	void ApplyMaterialSwap(RE::NiAVObject* a_object, const RE::BGSMaterialSwap* a_swap);

	// Named node anywhere under a_root, or nullptr.  Virtual dispatch, so the
	// engine's own NiNode implementation does the walking: no address id and no
	// struct offsets, unlike every other traversal in this plugin.
	[[nodiscard]] RE::NiAVObject* FindNode(RE::NiAVObject* a_root, const RE::BSFixedString& a_name);

	[[nodiscard]] bool CanTestActivationBlocked() noexcept;
	[[nodiscard]] bool IsActivationBlocked(RE::TESObjectREFR* a_ref);

	[[nodiscard]] bool CanReopenPipboy() noexcept;
	void PlayPipboyOpenAnim(RE::PipboyManager* a_manager, const RE::BSFixedString& a_menuName);

	// -----------------------------------------------------------------------
	// Animation clip timing.
	//
	// This walks raw Havok structures by byte offset.  Runtime-aware addresses
	// do NOT make class layouts portable - that is the "structureIndependence"
	// caveat in CommonLibF4RD's docs - so the offsets are only trusted on the
	// runtime families they were verified against.  Where they are not
	// verified, ReadCurrentClip() reports failure and the caller falls back to
	// a fixed delay instead of dereferencing guesses.
	// -----------------------------------------------------------------------

	struct ClipInfo
	{
		float currentTime{ 0.0f };
		float duration{ 0.0f };
		std::string name{};

		// An active generator can be identified by name before its animation
		// binding is reachable.  When this is false, `name` is trustworthy but
		// `duration` is not, and the caller must not compute a remaining time
		// from it.
		bool durationKnown{ false };
	};

	[[nodiscard]] bool CanReadClipInfo() noexcept;
	[[nodiscard]] bool ReadCurrentClip(RE::Actor* a_actor, ClipInfo& a_out);
}
