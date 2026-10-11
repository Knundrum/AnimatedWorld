#include "Hooks.h"

#include "Addresses.h"
#include "Diagnostics.h"
#include "Game.h"

#include "RE/Bethesda/BSInputDeviceManager.h"
#include "RE/Bethesda/FormFactory.h"
#include "RE/Bethesda/PipboyManager.h"
#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESDataHandler.h"
#include "RE/Bethesda/TESBoundObjects.h"
#include "RE/Bethesda/TESForms.h"
#include "RE/Bethesda/UI.h"

#include <algorithm>
#include <vector>

namespace AW::Hooks
{
	namespace
	{
		using namespace std::chrono_literals;

		// -------------------------------------------------------------------
		// Plugin content
		// -------------------------------------------------------------------
		constexpr auto ESP_NAME = "Animated World - Base.esp"sv;

		constexpr std::uint32_t FORMID_IDLE_STOP_FIX = 0x34D3A;
		constexpr std::uint32_t FORMID_ACTION_ACTIVATE = 0x12196;
		constexpr std::uint32_t FORMID_ACTION_ITEM_ADDED = 0x2B4D9;
		constexpr std::uint32_t FORMID_ACTION_EQUIP_ANIM = 0x18481;
		constexpr std::uint32_t FORMID_ACTION_FLASHLIGHT = 0x14F3E;
		constexpr std::uint32_t FORMID_GLOBAL_PIPBOY_EQUIP = 0x399BC;

		// -------------------------------------------------------------------
		// Timing
		// -------------------------------------------------------------------
		constexpr auto ANIMATION_SETTLE_DELAY = 100ms;
		constexpr auto GROUND_PICKUP_DELAY = 100ms;
		constexpr auto MATERIAL_SWAP_DELAY = 200ms;
		constexpr auto DYNAMIC_IDLE_TAIL = 200ms;

		// Pause after an IdleStop event before firing the item-added animation.
		// Longer than DYNAMIC_IDLE_TAIL on purpose: the timed path knows where
		// the clip actually ends and only needs to clear it, while IdleStop
		// fires as the idle begins unwinding, so the graph is still settling.
		// Too short and the engine replays the previous animation instead.
		// Tunable in game; see Diagnostics::PickupDelayOverride.
		constexpr auto IDLE_STOP_TAIL = 400ms;

		// How long to wait for an IdleStop event before giving up on it, where
		// the clip walk is unavailable (unverified Havok struct layout on
		// NG/AE).  This is a DEADLINE, not a delay: the animation normally
		// fires on the event, well before this.  Measured arrivals are ~475ms
		// on AE and ~720ms on OG, so this leaves room for a longer idle without
		// stalling noticeably when no event comes at all.
		//
		// Tunable in game without a rebuild; see Diagnostics::PickupDelayOverride.
		constexpr auto CLIP_UNREADABLE_FALLBACK = 1500ms;

		// The IdleStop fix used to latch on forever if the event never arrived,
		// so a later unrelated IdleStop would consume it.  It now expires.
		constexpr auto IDLE_STOP_TIMEOUT = 2s;

		// An armed animation whose clip never turns into DynamicIdle used to
		// poll every frame for the rest of the session.  Give it up eventually.
		constexpr auto ANIMATION_PENDING_TIMEOUT = 3s;

		// Enough to see a pickup's whole event vocabulary once; a cap so a long
		// session cannot grow this without bound.
		constexpr std::size_t MAX_REPORTED_PENDING_EVENTS = 16;

		constexpr auto DYNAMIC_IDLE_CLIP = "DynamicIdle"sv;
		constexpr auto EVENT_IDLE_STOP = "IdleStop"sv;
		constexpr auto EVENT_REEVALUATE = "ReevaluateGraphState"sv;
		constexpr auto MENU_PIPBOY = "PipboyMenu"sv;
		constexpr auto PIPBOY_INVENTORY_ANIM = "PipboyInv"sv;

		using Clock = std::chrono::steady_clock;

		// -------------------------------------------------------------------
		// Resolved plugin forms
		// -------------------------------------------------------------------
		RE::BGSKeyword* g_idleStopFixKeyword{ nullptr };
		RE::BGSAction* g_actionActivate{ nullptr };
		RE::BGSAction* g_actionItemAdded{ nullptr };
		RE::BGSAction* g_actionEquipAnim{ nullptr };
		RE::BGSAction* g_actionFlashlight{ nullptr };
		RE::TESGlobal* g_globalPipboyEquipAnims{ nullptr };

		// Dummy references retargeted at whichever item is being animated, so a
		// single BGSAction can drive an animation for any object.
		RE::TESObjectREFR* g_playerTarget{ nullptr };
		RE::TESObjectREFR* g_npcTarget{ nullptr };

		bool g_formsReady{ false };

		// -------------------------------------------------------------------
		// State machine
		// -------------------------------------------------------------------
		bool g_reopenPipboy{ false };

		bool g_idleStopFixArmed{ false };
		Clock::time_point g_idleStopFixExpiry{};

		bool g_animationPending{ false };
		Clock::time_point g_animationDeadline{};
		Clock::time_point g_animationExpiry{};

		// Only used where the clip cannot be timed: the point at which waiting
		// for an IdleStop event is given up and the animation fires anyway.
		Clock::time_point g_blindDeadline{};

		bool g_animationSoon{ false };
		Clock::time_point g_animationReady{};

		bool g_matSwapPending{ false };
		Clock::time_point g_matSwapDeadline{};

		bool g_itemFromGround{ false };

		// Set by the animation graph event sink when the idle that was playing
		// while an item-added animation is pending reports that it stopped.
		// This is how the wait is ended on a runtime that cannot time the clip:
		// an event, not a guess about how long a pickup takes.
		bool g_idleEndSeen{ false };

		// Distinct graph event names already reported while pending, so the
		// vocabulary can be read off an ordinary log without flooding it.
		std::vector<std::string> g_reportedPendingEvents;

		// The graph event sink depends on a COMPILE-TIME base-class offset for
		// BSTEventSink<BSAnimationGraphEvent> inside PlayerCharacter, which is
		// struct layout, not an address - the one kind of thing a runtime
		// database cannot make portable.  These report whether the hook is
		// entered at all and whether its identity check passes, because a
		// silent failure here looks exactly like "no events ever fire".
		std::size_t g_graphEventEntries{ 0 };
		bool g_loggedGraphEventMatch{ false };

		// The swap is remembered together with the item it belongs to, so a
		// swap left over from an earlier pickup can never be applied to an
		// unrelated item that arrived through a different code path.
		RE::BGSMaterialSwap* g_pendingSwap{ nullptr };
		RE::TESBoundObject* g_pendingSwapItem{ nullptr };

		bool g_graphEventHooked{ false };

		// Verbose state-machine logging, gated on the AnimatedWorld.tracehooks
		// marker file.  Cached so the per-frame path does not touch the disk.
		bool g_trace{ false };

		// Only log the clip name when it changes, or the per-frame poll floods
		// the log while an animation is settling.
		std::string g_lastTracedClip;

		// One-shot reporting flags.  The per-frame driver has several early
		// exits; when the item-added animation never fires, the only question
		// that matters is which one is taking every frame.  Each milestone is
		// reported once per session so an ordinary log answers it without the
		// trace marker.
		bool g_loggedNoPlayer{ false };
		bool g_loggedNoProcess{ false };
		bool g_loggedNoForms{ false };
		bool g_loggedDriverLive{ false };
		bool g_loggedArmed{ false };
		bool g_loggedFired{ false };
		bool g_loggedArmRequest{ false };
		bool g_loggedAddAcquiredEntry{ false };
		bool g_loggedHandlePlayerItemEntry{ false };
		bool g_loggedIdleDuration{ false };

		std::chrono::milliseconds g_clipUnreadableFallback{ CLIP_UNREADABLE_FALLBACK };

		// IDLE_STOP_TAIL, or the value from the override file.
		std::chrono::milliseconds g_idleStopTail{ IDLE_STOP_TAIL };

		// -------------------------------------------------------------------
		// Originals
		// -------------------------------------------------------------------
		using FnRunActorUpdates = void(__fastcall*)(void*);
		using FnAddAcquiredEvent = void(__fastcall*)(RE::PlayerCharacter*, RE::TESBoundObject*, RE::TESForm*, RE::TESObjectREFR*, std::int32_t);
		using FnActivateRef = bool(__fastcall*)(RE::TESObjectREFR*, RE::TESObjectREFR*, RE::TESBoundObject*, int, bool, bool, bool);
		using FnHandlePlayerItem = void(__fastcall*)(RE::TESBoundObject*, RE::ExtraDataList*, std::uint32_t);
		// Four arguments, confirmed by disassembling both callers: rcx = the
		// manager, rdx = the actor, r8 = a BGSObjectInstance*, r9 = a pointer to
		// a caller-built parameter block (count, slot, the equip flags).  The
		// eleven-argument form this used to carry happened to work on OG only
		// because a genuinely four-argument callee ignores the extra register
		// and stack slots; the flags it named were never actually delivered.
		using FnUseObject = bool(__fastcall*)(
			RE::ActorEquipManager*,
			RE::Actor*,
			const RE::BGSObjectInstance&,
			void*);
		using FnSetInputDeviceLightState = void(__fastcall*)(RE::BSInputDeviceManager*, std::uint32_t, bool);
		using FnProcessGraphEvent = RE::BSEventNotifyControl(__fastcall*)(
			RE::BSTEventSink<RE::BSAnimationGraphEvent>*,
			const RE::BSAnimationGraphEvent&,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>*);

		FnRunActorUpdates g_origRunActorUpdates{ nullptr };
		FnAddAcquiredEvent g_origAddAcquiredEvent{ nullptr };
		FnActivateRef g_origActivateRef{ nullptr };
		FnHandlePlayerItem g_origHandlePlayerItem{ nullptr };
		FnUseObject g_origUseObject{ nullptr };
		FnSetInputDeviceLightState g_origSetInputDeviceLightState{ nullptr };
		FnProcessGraphEvent g_origProcessGraphEvent{ nullptr };

		// -------------------------------------------------------------------
		// Helpers
		// -------------------------------------------------------------------

		[[nodiscard]] RE::TESObjectREFR* CreateDummyReference()
		{
			auto* factory =
				RE::ConcreteFormFactory<RE::TESObjectREFR, RE::ENUM_FORM_ID::kREFR>::GetFormFactory();
			return factory ? factory->Create() : nullptr;
		}

		// Resolves the plugin's forms once.  Returns false while the data
		// handler is not up yet or the esp is missing, and the hooks then do
		// nothing rather than dereferencing null actions.
		bool EnsureFormsResolved()
		{
			if (g_formsReady) {
				return true;
			}

			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				return false;
			}

			if (!g_playerTarget) {
				g_playerTarget = CreateDummyReference();
			}
			if (!g_npcTarget) {
				g_npcTarget = CreateDummyReference();
			}

			if (!g_idleStopFixKeyword) {
				g_idleStopFixKeyword = dataHandler->LookupForm<RE::BGSKeyword>(FORMID_IDLE_STOP_FIX, ESP_NAME);
			}
			if (!g_actionActivate) {
				g_actionActivate = dataHandler->LookupForm<RE::BGSAction>(FORMID_ACTION_ACTIVATE, ESP_NAME);
			}
			if (!g_actionItemAdded) {
				g_actionItemAdded = dataHandler->LookupForm<RE::BGSAction>(FORMID_ACTION_ITEM_ADDED, ESP_NAME);
			}
			if (!g_actionEquipAnim) {
				g_actionEquipAnim = dataHandler->LookupForm<RE::BGSAction>(FORMID_ACTION_EQUIP_ANIM, ESP_NAME);
			}
			if (!g_actionFlashlight) {
				g_actionFlashlight = dataHandler->LookupForm<RE::BGSAction>(FORMID_ACTION_FLASHLIGHT, ESP_NAME);
			}
			if (!g_globalPipboyEquipAnims) {
				g_globalPipboyEquipAnims = dataHandler->LookupForm<RE::TESGlobal>(FORMID_GLOBAL_PIPBOY_EQUIP, ESP_NAME);
			}

			g_formsReady =
				g_playerTarget && g_npcTarget &&
				g_actionActivate && g_actionItemAdded && g_actionEquipAnim && g_actionFlashlight;

			static bool reported = false;
			if (g_formsReady && !reported) {
				reported = true;
				logger::info(
					"forms resolved: activate={} itemAdded={} equip={} flashlight={} keyword={} pipboyGlobal={}",
					g_actionActivate != nullptr,
					g_actionItemAdded != nullptr,
					g_actionEquipAnim != nullptr,
					g_actionFlashlight != nullptr,
					g_idleStopFixKeyword != nullptr,
					g_globalPipboyEquipAnims != nullptr);
			}

			return g_formsReady;
		}

		void ArmIdleStopFix()
		{
			g_idleStopFixArmed = true;
			g_idleStopFixExpiry = Clock::now() + IDLE_STOP_TIMEOUT;
		}

		void ArmAnimation()
		{
			if (g_trace) {
				logger::info("[aw] animation armed (waiting for the activate animation to finish)");
			}

			if (!g_loggedArmRequest) {
				g_loggedArmRequest = true;
				logger::info("[aw] item-added animation requested for the first time");
			}

			const auto now = Clock::now();
			g_animationDeadline = now + ANIMATION_SETTLE_DELAY;
			g_animationExpiry = now + ANIMATION_PENDING_TIMEOUT;
			g_animationPending = true;
			g_idleEndSeen = false;
			g_blindDeadline = now + g_clipUnreadableFallback;
		}

		[[nodiscard]] RE::BGSMaterialSwap* SwapFormFor(RE::TESBoundObject* a_item)
		{
			if (!a_item) {
				return nullptr;
			}

			switch (a_item->formType.get()) {
			case RE::ENUM_FORM_ID::kBOOK:
				return static_cast<RE::TESObjectBOOK*>(a_item)->swapForm;
			case RE::ENUM_FORM_ID::kMISC:
				return static_cast<RE::TESObjectMISC*>(a_item)->swapForm;
			default:
				return nullptr;
			}
		}

		void ClearPendingSwap()
		{
			g_pendingSwap = nullptr;
			g_pendingSwapItem = nullptr;
		}

		[[nodiscard]] bool PipboyMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->GetMenuOpen(RE::BSFixedString{ MENU_PIPBOY });
		}

		// -------------------------------------------------------------------
		// BSTEventSink<BSAnimationGraphEvent>::ProcessEvent, patched into the
		// player's vtable.  No address id needed - the sink slot index is part
		// of the interface, so this works on every runtime.
		// -------------------------------------------------------------------
		RE::BSEventNotifyControl HookedProcessGraphEvent(
			RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this,
			const RE::BSAnimationGraphEvent& a_event,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			if (!g_origProcessGraphEvent) {
				return RE::BSEventNotifyControl::kContinue;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			const auto* playerSink =
				player ? static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(player) : nullptr;
			const bool isPlayer = a_this && playerSink && playerSink == a_this;

			if (g_graphEventEntries < 3) {
				++g_graphEventEntries;
				logger::info(
					"[aw] graph event sink entered: this={} playerSink={} match={} event='{}'",
					static_cast<const void*>(a_this),
					static_cast<const void*>(playerSink),
					isPlayer,
					a_event.animEvent.c_str() ? a_event.animEvent.c_str() : "");
			}

			if (!isPlayer && !g_loggedGraphEventMatch && g_graphEventEntries >= 3) {
				g_loggedGraphEventMatch = true;
				logger::warn(
					"[aw] graph event sink is firing but never for the player - the "
					"BSTEventSink base offset inside PlayerCharacter does not match this runtime");
			}

			if (isPlayer) {
				const auto* tag = a_event.animEvent.c_str();
				const auto event = tag ? std::string_view{ tag } : std::string_view{};

				// The window has to stay open through g_animationSoon as well.
				// g_animationPending ends the moment the driver arms, about
				// 100ms in, so watching only that showed the first few events of
				// a pickup and none of the ones that end it.
				if ((g_animationPending || g_animationSoon) && !event.empty()) {
					// Only while still waiting.  An IdleStop arriving after the
					// animation has already been scheduled belongs to whatever
					// comes next, not to this cycle.
					if (event == EVENT_IDLE_STOP && g_animationPending) {
						g_idleEndSeen = true;
					}

					if (g_reportedPendingEvents.size() < MAX_REPORTED_PENDING_EVENTS &&
						std::find(g_reportedPendingEvents.begin(), g_reportedPendingEvents.end(), event) ==
							g_reportedPendingEvents.end()) {
						g_reportedPendingEvents.emplace_back(event);
						logger::info("[aw] graph event while pending: '{}'", event);
					}
				}

				if (event == EVENT_IDLE_STOP && g_idleStopFixArmed) {
					const bool holdsFixedItem =
						g_idleStopFixKeyword &&
						Game::CanTestWornKeyword() &&
						Game::WornHasKeyword(player, g_idleStopFixKeyword);

					player->UpdateAnimation(holdsFixedItem ? 2.0f : 0.0f);
					g_idleStopFixArmed = false;
				} else if (event == EVENT_REEVALUATE && g_reopenPipboy) {
					g_reopenPipboy = false;

					auto* pipboy = RE::PipboyManager::GetSingleton();
					if (pipboy && PipboyMenuOpen()) {
						Game::PlayPipboyOpenAnim(pipboy, RE::BSFixedString{ PIPBOY_INVENTORY_ANIM });
					}
				}
			}

			return g_origProcessGraphEvent(a_this, a_event, a_source);
		}

		void InstallGraphEventHook()
		{
			if (g_graphEventHooked) {
				return;
			}

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetFullyLoaded3D()) {
				return;
			}

			using Sink = RE::BSTEventSink<RE::BSAnimationGraphEvent>;
			auto* sink = static_cast<Sink*>(player);
			if (!sink) {
				return;
			}

			auto* vtable = *reinterpret_cast<std::uintptr_t**>(sink);
			if (!vtable) {
				return;
			}

			// Slot 0 is the destructor, slot 1 is ProcessEvent.
			constexpr std::size_t PROCESS_EVENT_SLOT = 1;

			g_origProcessGraphEvent =
				reinterpret_cast<FnProcessGraphEvent>(vtable[PROCESS_EVENT_SLOT]);
			if (!g_origProcessGraphEvent) {
				return;
			}

			REL::safe_write(
				reinterpret_cast<std::uintptr_t>(std::addressof(vtable[PROCESS_EVENT_SLOT])),
				reinterpret_cast<std::uintptr_t>(&HookedProcessGraphEvent));

			g_graphEventHooked = true;
			logger::info(
				"player animation graph event sink hooked (sink is +{:#x} into PlayerCharacter)",
				reinterpret_cast<std::uintptr_t>(sink) - reinterpret_cast<std::uintptr_t>(player));
		}

		// -------------------------------------------------------------------
		// Hooks
		// -------------------------------------------------------------------

		// Silence from every early-exit line AND no "driver live" line means the
		// hook is not running at all, which is a different problem from any of
		// the gates below.
		void LogOnce(bool& a_flag, std::string_view a_message)
		{
			if (!a_flag) {
				a_flag = true;
				logger::info("[aw] {}", a_message);
			}
		}

		void __fastcall HookedRunActorUpdates(void* a_this)
		{
			g_origRunActorUpdates(a_this);

			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetFullyLoaded3D()) {
				LogOnce(g_loggedNoPlayer, "driver: player or player 3D not ready"sv);
				return;
			}

			// currentProcess and middleHigh are STRUCT OFFSETS, not addresses.
			// CommonLibF4RD pins Actor at sizeof 0x490 with currentProcess at
			// 0x300 and gives no per-runtime layout, so on a runtime where that
			// layout is wrong this reads an unrelated field and is false every
			// frame.  It used to return here, which silently disabled the entire
			// state machine - the only thing that actually needs the process is
			// the clip walk, and that does its own null checks and is switched
			// off wherever the Havok offsets are unverified.  So it is now
			// recorded and ignored rather than fatal.
			if (!player->currentProcess || !player->currentProcess->middleHigh) {
				LogOnce(g_loggedNoProcess, "driver: currentProcess/middleHigh unavailable - continuing anyway"sv);
			}

			InstallGraphEventHook();

			if (!EnsureFormsResolved()) {
				LogOnce(g_loggedNoForms, "driver: forms not resolved"sv);
				return;
			}

			LogOnce(g_loggedDriverLive, "driver live - state machine running"sv);

			const auto now = Clock::now();

			// An armed IdleStop fix that never saw its event must not survive
			// into an unrelated animation.
			if (g_idleStopFixArmed && now >= g_idleStopFixExpiry) {
				g_idleStopFixArmed = false;
			}

			if (g_animationPending && now >= g_animationExpiry) {
				if (g_trace) {
					logger::info("[aw] pending animation timed out without a usable clip");
				}
				g_animationPending = false;
				ClearPendingSwap();
			}

			if (g_animationPending && !g_matSwapPending && now >= g_animationDeadline) {
				Game::ClipInfo clip;
				const bool haveClip = Game::ReadCurrentClip(player, clip);

				if (g_trace && clip.name != g_lastTracedClip) {
					g_lastTracedClip = clip.name;
					logger::info(
						"[aw] clip: have={} name='{}' t={:.3f} dur={:.3f} durKnown={}",
						haveClip,
						clip.name,
						clip.currentTime,
						clip.duration,
						clip.durationKnown);
				}

				// These two conditions are NOT mutually exclusive, and the
				// order matters: a playing DynamicIdle wins over the ground
				// shortcut.  The pickup/activate animation has to finish before
				// the item-added animation starts, otherwise the new action is
				// fired into a still-running idle and is swallowed - which looks
				// exactly like "no animation played".
				bool arm = false;
				auto delay = GROUND_PICKUP_DELAY;
				const char* reason = "";

				if (g_itemFromGround) {
					arm = true;
					delay = GROUND_PICKUP_DELAY;
					reason = "itemFromGround";
				}

				if (haveClip && clip.name == DYNAMIC_IDLE_CLIP) {
					if (clip.durationKnown) {
						// Reported once so a runtime that CAN time the clip
						// tells us what to set the blind wait to on one that
						// cannot.
						if (!g_loggedIdleDuration) {
							g_loggedIdleDuration = true;
							logger::info(
								"[aw] DynamicIdle measured: duration={:.3f}s at t={:.3f}s"
								" - a blind wait needs to cover this",
								clip.duration,
								clip.currentTime);
						}

						const auto remaining =
							std::chrono::milliseconds{
								static_cast<std::int64_t>((clip.duration - clip.currentTime) * 1000.0f)
							} +
							DYNAMIC_IDLE_TAIL;

						arm = true;
						delay = remaining.count() > 0 ? remaining : DYNAMIC_IDLE_TAIL;
						reason = "DynamicIdle";
					} else {
						// The clip is identified but not yet timeable.  Keep
						// polling instead of cutting the animation short; the
						// binding normally resolves within a frame or two.
						arm = false;
						reason = "";
					}
				}

				if (!Game::CanReadClipInfo()) {
					// No clip timing here, so the length of the pickup cannot be
					// computed - but it can still be OBSERVED.  Keep waiting
					// until the graph reports IdleStop, then add the same tail
					// the timed path uses.  That tracks a pickup of any length
					// instead of assuming one.
					//
					// The fixed wait is now a deadline for giving up on the
					// event, not the delay itself.  Arming on it immediately
					// was the flaw in the first attempt: the event arrived
					// afterwards and there was nothing left to tell.
					//
					// GROUND_PICKUP_DELAY must not survive to here either way.
					// It is a floor that only makes sense when the DynamicIdle
					// branch can still raise it; on its own it fires the action
					// into the running pickup idle, which swallows it.
					if (g_idleEndSeen) {
						arm = true;
						delay = g_idleStopTail;
						reason = "idle stopped";
					} else if (now >= g_blindDeadline) {
						arm = true;
						delay = g_idleStopTail;
						reason = "no IdleStop before the deadline";
					} else {
						arm = false;
						reason = "";
					}
				}

				if (arm) {
					g_animationPending = false;
					g_animationSoon = true;
					g_animationReady = now + delay;

					if (!g_loggedArmed) {
						g_loggedArmed = true;
						logger::info("[aw] driver: first arm via {}, +{}ms", reason, delay.count());
					}

					if (g_trace) {
						logger::info("[aw] armed via {}, +{}ms", reason, delay.count());
					}
				}
			}

			if (g_animationSoon && !g_matSwapPending && now >= g_animationReady) {
				g_animationSoon = false;
				g_lastTracedClip.clear();

				if (g_actionItemAdded && g_playerTarget) {
					const bool played = Game::PlayAction(player, g_actionItemAdded, g_playerTarget);

					if (!g_loggedFired) {
						g_loggedFired = true;
						logger::info("[aw] driver: first item-added action, played={}", played);
					}

					if (g_trace) {
						logger::info(
							"[aw] item-added action played={} target item={:08X}",
							played,
							g_playerTarget->data.objectReference ?
								g_playerTarget->data.objectReference->formID :
								0u);
					}

					const bool swapMatchesTarget =
						g_pendingSwap &&
						g_pendingSwapItem &&
						g_playerTarget->data.objectReference == g_pendingSwapItem;

					if (swapMatchesTarget && Game::CanApplyMaterialSwap()) {
						g_matSwapPending = true;
						g_matSwapDeadline = now + MATERIAL_SWAP_DELAY;
					} else {
						ClearPendingSwap();
					}
				}
			}

			if (g_matSwapPending && now >= g_matSwapDeadline) {
				g_matSwapPending = false;

				if (g_pendingSwap) {
					Game::ApplyMaterialSwap(player->Get3D(), g_pendingSwap);
				}

				ClearPendingSwap();
			}
		}

		bool __fastcall HookedActivateRef(
			RE::TESObjectREFR* a_target,
			RE::TESObjectREFR* a_activator,
			RE::TESBoundObject* a_item,
			int a_count,
			bool a_force,
			bool a_silent,
			bool a_other)
		{
			const auto callOriginal = [&] {
				return g_origActivateRef(a_target, a_activator, a_item, a_count, a_force, a_silent, a_other);
			};

			if (Game::IsActivationBlocked(a_target)) {
				return callOriginal();
			}

			g_itemFromGround = false;

			// Every other hook here refuses to play an idle before the player's
			// 3D exists.  These two did not, and AddAcquiredEvent in particular
			// fires during the intro while the player is still being built, so
			// the check is now applied consistently.
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && player->GetFullyLoaded3D() && a_target &&
				EnsureFormsResolved() && g_actionActivate) {
				if (player->weaponState != RE::WEAPON_STATE::kSheathed) {
					ArmIdleStopFix();
				}

				if (Game::PlayAction(player, g_actionActivate, a_target)) {
					g_itemFromGround = true;
				}
			}

			return callOriginal();
		}

		void __fastcall HookedAddAcquiredEvent(
			RE::PlayerCharacter* a_player,
			RE::TESBoundObject* a_item,
			RE::TESForm* a_source,
			RE::TESObjectREFR* a_container,
			std::int32_t a_acquireType)
		{
			const auto callOriginal = [&] {
				g_origAddAcquiredEvent(a_player, a_item, a_source, a_container, a_acquireType);
			};

			auto* player = RE::PlayerCharacter::GetSingleton();

			if (!g_loggedAddAcquiredEntry) {
				g_loggedAddAcquiredEntry = true;
				logger::info(
					"[aw] AddAcquiredEvent hook reached: item={} loaded3D={} forms={}",
					a_item != nullptr,
					player && player->GetFullyLoaded3D(),
					g_formsReady);
			}

			// The original fell through this guard without returning, then went
			// on to use the null pointers it had just rejected.
			if (!player || !player->GetFullyLoaded3D() || !a_item ||
				!EnsureFormsResolved() || !g_playerTarget || !g_actionActivate) {
				callOriginal();
				return;
			}

			g_playerTarget->data.objectReference = a_item;

			if (player->weaponState != RE::WEAPON_STATE::kSheathed) {
				ArmIdleStopFix();
			}

			const bool played = Game::PlayAction(player, g_actionActivate, g_playerTarget);
			ArmAnimation();

			if (g_trace) {
				logger::info(
					"[aw] AddAcquiredEvent item={:08X} activateAction played={} fromGround={}",
					a_item->formID,
					played,
					g_itemFromGround);
			}

			callOriginal();
		}

		void __fastcall HookedHandlePlayerItem(
			RE::TESBoundObject* a_item,
			RE::ExtraDataList* a_extra,
			std::uint32_t a_count)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();

			if (!g_loggedHandlePlayerItemEntry) {
				g_loggedHandlePlayerItemEntry = true;
				logger::info(
					"[aw] HandlePlayerItem hook reached: item={} loaded3D={} forms={}",
					a_item != nullptr,
					player && player->GetFullyLoaded3D(),
					g_formsReady);
			}

			if (g_trace) {
				logger::info(
					"[aw] HandlePlayerItem item={:08X} loaded3D={} formsReady={}",
					a_item ? a_item->formID : 0u,
					player && player->GetFullyLoaded3D(),
					g_formsReady);
			}

			if (player && player->GetFullyLoaded3D() && a_item &&
				EnsureFormsResolved() && g_playerTarget) {
				g_playerTarget->data.objectReference = a_item;

				g_pendingSwap = SwapFormFor(a_item);
				g_pendingSwapItem = g_pendingSwap ? a_item : nullptr;

				ArmAnimation();
			}

			g_origHandlePlayerItem(a_item, a_extra, a_count);
		}

		bool __fastcall HookedUseObject(
			RE::ActorEquipManager* a_this,
			RE::Actor* a_actor,
			const RE::BGSObjectInstance& a_object,
			void* a_params)
		{
			const auto callOriginal = [&] {
				return g_origUseObject(a_this, a_actor, a_object, a_params);
			};

			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* baseForm = static_cast<RE::TESBoundObject*>(a_object.object);

			if (!a_actor || !player || !baseForm || !a_actor->GetFullyLoaded3D()) {
				return callOriginal();
			}

			const bool animatable =
				baseForm->formType == RE::ENUM_FORM_ID::kALCH ||
				baseForm->formType == RE::ENUM_FORM_ID::kARMO;

			if (!animatable || !EnsureFormsResolved() || !g_actionEquipAnim) {
				return callOriginal();
			}

			if (a_actor == player) {
				if (g_playerTarget) {
					g_playerTarget->data.objectReference = baseForm;

					if (PipboyMenuOpen()) {
						const bool enabled =
							g_globalPipboyEquipAnims && g_globalPipboyEquipAnims->value > 0.0f;

						// Playing the equip animation tears the Pip-Boy down, so
						// only do it if we can put it back afterwards.
						if (enabled && Game::CanReopenPipboy()) {
							static_cast<void>(Game::PlayAction(player, g_actionEquipAnim, g_playerTarget));
							g_reopenPipboy = true;
						}
					} else {
						static_cast<void>(Game::PlayAction(player, g_actionEquipAnim, g_playerTarget));
					}
				}
			} else if (baseForm->formType == RE::ENUM_FORM_ID::kALCH && g_npcTarget) {
				g_npcTarget->data.objectReference = baseForm;
				static_cast<void>(Game::PlayAction(a_actor, g_actionEquipAnim, g_npcTarget));
			}

			return callOriginal();
		}

		void __fastcall HookedSetInputDeviceLightState(
			RE::BSInputDeviceManager* a_manager,
			std::uint32_t a_state,
			bool a_on)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();

			// The original fired the flashlight animation unconditionally, which
			// includes calls made while the player has no 3D (loading, main menu).
			if (player && player->GetFullyLoaded3D() && EnsureFormsResolved() && g_actionFlashlight) {
				ArmIdleStopFix();
				static_cast<void>(Game::PlayAction(player, g_actionFlashlight, player));
			}

			g_origSetInputDeviceLightState(a_manager, a_state, a_on);
		}

		// -------------------------------------------------------------------
		// Installation
		// -------------------------------------------------------------------

		template <class F>
		[[nodiscard]] bool InstallCall(Addresses::Site a_site, F& a_original, F a_hook)
		{
			const auto address = Addresses::ResolveSite(a_site);
			if (!address) {
				return false;
			}

			auto& trampoline = F4SE::GetTrampoline();
			a_original = reinterpret_cast<F>(trampoline.write_call<5>(*address, a_hook));
			logger::info("installed {} hook", Addresses::GetSite(a_site).name);
			return true;
		}

		template <class F>
		[[nodiscard]] bool InstallBranch(Addresses::Site a_site, F& a_original, F a_hook)
		{
			const auto address = Addresses::ResolveSite(a_site);
			if (!address) {
				return false;
			}

			auto& trampoline = F4SE::GetTrampoline();
			a_original = reinterpret_cast<F>(trampoline.write_branch<5>(*address, a_hook));
			logger::info("installed {} hook", Addresses::GetSite(a_site).name);
			return true;
		}
	}

	bool Install()
	{
		g_trace = Diagnostics::HookTraceEnabled();
		if (g_trace) {
			logger::info("[aw] hook tracing enabled");
		}

		if (const auto tuned = Diagnostics::PickupDelayOverride()) {
			g_idleStopTail = *tuned;

			// The deadline only matters when no IdleStop arrives at all, so it
			// just has to stay clear of the tail rather than be tuned itself.
			g_clipUnreadableFallback = (std::max)(CLIP_UNREADABLE_FALLBACK, *tuned + CLIP_UNREADABLE_FALLBACK / 2);

			logger::info("[aw] pause after IdleStop overridden to {}ms", tuned->count());
		}

		// Without the per-frame driver none of the timed follow-ups can run, so
		// there is no point installing anything else.
		if (!InstallCall(Addresses::Site::kRunActorUpdates, g_origRunActorUpdates, &HookedRunActorUpdates)) {
			logger::error("RunActorUpdates hook unavailable - AnimatedWorld will stay inactive");
			return false;
		}

		static_cast<void>(InstallCall(
			Addresses::Site::kAddAcquiredEvent, g_origAddAcquiredEvent, &HookedAddAcquiredEvent));

		static_cast<void>(InstallCall(
			Addresses::Site::kHandlePlayerItem, g_origHandlePlayerItem, &HookedHandlePlayerItem));

		static_cast<void>(InstallCall(
			Addresses::Site::kUseObject, g_origUseObject, &HookedUseObject));

		// Activation needs the blocked-ref test; without it the hook would fire
		// the animation on references the game refuses to activate.
		if (Game::CanTestActivationBlocked()) {
			static_cast<void>(InstallCall(
				Addresses::Site::kActivateRef, g_origActivateRef, &HookedActivateRef));
		} else {
			logger::warn("ActivateRef hook skipped: IsActivationBlocked is unavailable");
		}

		static_cast<void>(InstallBranch(
			Addresses::Site::kSetInputDeviceLightState,
			g_origSetInputDeviceLightState,
			&HookedSetInputDeviceLightState));

		return true;
	}
}
