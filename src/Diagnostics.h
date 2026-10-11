#pragma once

// ---------------------------------------------------------------------------
// Development helper for porting the plugin to another runtime family.
//
// Enable it by creating an empty marker file next to the F4SE log:
//
//     Documents\My Games\Fallout4\F4SE\AnimatedWorld.findcallsites
//
// On startup, BEFORE any hook is installed, this decodes the branch sitting at
// each hook site and reports which function it calls, as a Runtime Database id.
// That id is what a HookSite's `callsiteTarget` wants: once it is filled in,
// CommonLibF4RD finds the callsite itself with REL::AUTO_CALLSITE and the
// hardcoded interior offsets stop mattering on every runtime.
//
// Run it once on OG to learn the callee ids, then look those callees up rather
// than the hook owners - they are ordinary named functions, so several are
// likely already dual-keyed inside CommonLibF4RD's own headers.
// ---------------------------------------------------------------------------

namespace AW::Diagnostics
{
	[[nodiscard]] bool CallsiteDumpRequested();

	// Verbose per-step logging of the animation state machine.  Enable with an
	// empty marker file next to the F4SE log:
	//
	//     Documents\My Games\Fallout4\F4SE\AnimatedWorld.tracehooks
	[[nodiscard]] bool HookTraceEnabled();

	// Must run before Hooks::Install(), or it will decode our own trampolines.
	void DumpCallsiteTargets();

	// Runtime Database id owning an rva, or nullopt when the rva is not the
	// start of a known function.  Used to report the real id behind a raw
	// address override, so the override can be replaced by an id next build.
	[[nodiscard]] std::optional<std::uint64_t> IdForRva(std::size_t a_rva);

	// Milliseconds to pause after the animation graph reports IdleStop, before
	// the item-added animation is played, on a runtime where the clip walk is
	// unavailable.  Raise it if the previous animation replays instead.  Read
	// from a one-line file next to the log:
	//
	//     Documents\My Games\Fallout4\F4SE\AnimatedWorld.pickupdelay
	//
	// Lets the wait be tuned in game rather than rebuilt.  Values outside
	// 50..10000 are ignored.
	[[nodiscard]] std::optional<std::chrono::milliseconds> PickupDelayOverride();
}
