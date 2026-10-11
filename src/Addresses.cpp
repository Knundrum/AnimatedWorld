#include "Addresses.h"

#include "Diagnostics.h"

namespace AW::Addresses
{
	namespace
	{
		[[nodiscard]] REL::RuntimeFamily CurrentFamily() noexcept
		{
			return REL::runtime_family(REL::Module::get().version());
		}

		[[nodiscard]] std::ptrdiff_t OffsetForRuntime(const HookSite& a_site) noexcept
		{
			switch (CurrentFamily()) {
			case REL::RuntimeFamily::kOG:
				return a_site.ogOffset;
			case REL::RuntimeFamily::kNG:
				return a_site.ngOffset;
			case REL::RuntimeFamily::kAE:
			default:
				return a_site.aeOffset;
			}
		}

		[[nodiscard]] std::uintptr_t OwnerRvaForRuntime(const HookSite& a_site) noexcept
		{
			switch (CurrentFamily()) {
			case REL::RuntimeFamily::kOG:
				return a_site.ownerRva.og;
			case REL::RuntimeFamily::kNG:
				return a_site.ownerRva.ng;
			case REL::RuntimeFamily::kAE:
			default:
				return a_site.ownerRva.ae;
			}
		}

		// Absolute target of the 5-byte E8/E9 branch at a_address, or nullopt
		// when the byte there is not that branch.
		[[nodiscard]] std::optional<std::uintptr_t> DecodeBranchTarget(
			std::uintptr_t a_address,
			REL::AutoCallsiteBranch a_branch) noexcept
		{
			const std::uint8_t expected =
				a_branch == REL::AutoCallsiteBranch::kJump ? std::uint8_t{ 0xE9 } : std::uint8_t{ 0xE8 };

			if (*reinterpret_cast<const std::uint8_t*>(a_address) != expected) {
				return std::nullopt;
			}

			const auto displacement = *reinterpret_cast<const std::int32_t*>(a_address + 1);
			return a_address + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement));
		}

		// The id actually used on this runtime, for logging.
		[[nodiscard]] std::uint64_t IDForRuntime(const REL::ID& a_id) noexcept
		{
			return a_id.id();
		}

		// Strips every id except the running family's.
		//
		// IDDatabase::resolve() on OG tries ae_id() as a runtime pattern FIRST
		// and only falls back to the OG legacy table if that misses.  So a real
		// AE id sitting in the AE slot can pattern-match somewhere inside the OG
		// executable and win, and the hook is then written into an unrelated
		// function with no error reported.  Handing resolve() an ID that carries
		// only the current family's number removes that whole failure mode: on
		// OG the empty AE slot cannot match, so the legacy table always decides.
		[[nodiscard]] REL::ID IsolateForRuntime(const REL::ID& a_id) noexcept
		{
			switch (CurrentFamily()) {
			case REL::RuntimeFamily::kOG:
				return REL::ID{ a_id.og_id(), REL::ID::INVALID_ID };
			case REL::RuntimeFamily::kNG:
				return REL::ID{ REL::ID::INVALID_ID, a_id.ng_id(), REL::ID::INVALID_ID };
			case REL::RuntimeFamily::kAE:
			default:
				return REL::ID{ a_id.ae_id() };
			}
		}
	}

	std::string_view RuntimeFamilyName() noexcept
	{
		switch (CurrentFamily()) {
		case REL::RuntimeFamily::kOG:
			return "OG"sv;
		case REL::RuntimeFamily::kNG:
			return "NG"sv;
		case REL::RuntimeFamily::kAE:
		default:
			return "AE"sv;
		}
	}

	bool HasIDForRuntime(const REL::ID& a_id) noexcept
	{
		return IDForRuntime(a_id) != REL::ID::INVALID_ID;
	}

	std::optional<std::uintptr_t> ResolveFunction(std::string_view a_name, const REL::ID& a_id)
	{
		if (!HasIDForRuntime(a_id)) {
			logger::warn(
				"{}: no {} id supplied - feature unavailable on this runtime",
				a_name,
				RuntimeFamilyName());
			return std::nullopt;
		}

		const auto result = REL::IDDatabase::get().resolve(IsolateForRuntime(a_id));
		if (!result) {
			logger::error(
				"{}: id {} could not be resolved ({})",
				a_name,
				IDForRuntime(a_id),
				REL::id_resolve_status_text(result.status));
			return std::nullopt;
		}

		return REL::Module::get().base() + *result.rva;
	}

	std::optional<std::uintptr_t> ResolveSite(Site a_site)
	{
		const auto& site = GetSite(a_site);

		// A raw owner rva replaces id resolution entirely, and is only trusted
		// when the branch it selects calls the function this site expects.
		if (const auto ownerRva = OwnerRvaForRuntime(site); ownerRva != 0) {
			const auto offset = OffsetForRuntime(site);
			if (offset == UNKNOWN_OFFSET) {
				logger::warn("{}: owner rva given without a {} offset - hook not installed", site.name, RuntimeFamilyName());
				return std::nullopt;
			}

			const auto expected = ResolveFunction(site.name, site.callsiteTarget);
			if (!expected) {
				logger::warn("{}: owner rva cannot be verified without a callee id - hook not installed", site.name);
				return std::nullopt;
			}

			const auto address = REL::Module::get().base() + ownerRva + static_cast<std::uintptr_t>(offset);
			const auto actual = DecodeBranchTarget(address, site.branch);

			if (!actual || *actual != *expected) {
				logger::warn(
					"{}: owner rva {:#x} +{:#x} does not branch to the expected callee on this build "
					"- hook not installed",
					site.name,
					ownerRva,
					offset);
				return std::nullopt;
			}

			if (const auto id = Diagnostics::IdForRva(ownerRva)) {
				logger::info("{}: owner rva {:#x} = id {} - replace the rva with this id", site.name, ownerRva, *id);
			} else {
				logger::info("{}: owner rva {:#x} verified (no id maps to it)", site.name, ownerRva);
			}

			return address;
		}

		const auto owner = ResolveFunction(site.name, site.owner);
		if (!owner) {
			return std::nullopt;
		}

		// Preferred: let CommonLibF4RD find the call itself.  Survives a shifted
		// function body or inserted instructions, and fails loudly rather than
		// guessing when the call is gone or ambiguous.
		if (HasIDForRuntime(site.callsiteTarget)) {
			const auto calls = REL::resolve_callsites(
				IsolateForRuntime(site.owner),
				IsolateForRuntime(site.callsiteTarget),
				site.branch);

			if (calls && calls.rvas.size() == 1) {
				logger::info(
					"{}: callsite discovered automatically at +{:#x}",
					site.name,
					calls.offsets.empty() ? std::ptrdiff_t{ 0 } : calls.offsets.front());
				return REL::Module::get().base() + calls.rvas.front();
			}

			logger::warn(
				"{}: automatic callsite unusable ({}, {} match(es)) - falling back to the fixed offset",
				site.name,
				REL::id_resolve_status_text(calls.status),
				calls.rvas.size());

			// With several candidates the fixed offset has to pick one of them
			// blind, so name them: the right site is the one whose offset the
			// table already carries, and a fixed offset that is NOT in this list
			// is pointing somewhere the callee is never called from.
			for (const auto candidate : calls.offsets) {
				logger::warn("{}:   candidate offset +{:#x}", site.name, candidate);
			}
		}

		const auto offset = OffsetForRuntime(site);
		if (offset == UNKNOWN_OFFSET) {
			logger::warn(
				"{}: no {} interior offset supplied - hook not installed",
				site.name,
				RuntimeFamilyName());
			return std::nullopt;
		}

		const auto address = *owner + static_cast<std::uintptr_t>(offset);

		// A fixed offset is a guess about a specific build.  If it is wrong the
		// trampoline writes five bytes into the middle of an instruction, which
		// corrupts the function and usually crashes somewhere unrelated.  The
		// branch opcode is a cheap check that catches almost every such mistake,
		// so a bad offset disables one feature instead of the process.
		const std::uint8_t expected =
			site.branch == REL::AutoCallsiteBranch::kJump ? std::uint8_t{ 0xE9 } : std::uint8_t{ 0xE8 };
		const auto actual = *reinterpret_cast<const std::uint8_t*>(address);

		if (actual != expected) {
			logger::warn(
				"{}: byte at +{:#x} is {:#04x}, expected {:#04x} - the offset does not "
				"point at a branch on this runtime, hook not installed",
				site.name,
				offset,
				actual,
				expected);
			return std::nullopt;
		}

		return address;
	}

	void LogCapabilityReport()
	{
		const auto describe = [](std::string_view a_name, const REL::ID& a_id) {
			if (!HasIDForRuntime(a_id)) {
				logger::info("  {:<26} MISSING  (no {} id)", a_name, RuntimeFamilyName());
				return;
			}

			const auto result = REL::IDDatabase::get().resolve(IsolateForRuntime(a_id));
			if (result) {
				logger::info(
					"  {:<26} ok       id {} -> rva {:#x}",
					a_name,
					IDForRuntime(a_id),
					*result.rva);
			} else {
				logger::info(
					"  {:<26} FAILED   id {} ({})",
					a_name,
					IDForRuntime(a_id),
					REL::id_resolve_status_text(result.status));
			}
		};

		logger::info(
			"AnimatedWorld address report - runtime {} (family {})",
			REL::Module::get().version().string(),
			RuntimeFamilyName());

		logger::info(" functions:");
		describe("PlayAction"sv, PlayAction);
		describe("ApplyMaterialSwap"sv, ApplyMaterialSwap);
		describe("IsActivationBlocked"sv, IsActivationBlocked);
		describe("WornHasKeyword"sv, WornHasKeyword);
		describe("PlayPipboyOpenAnim"sv, PlayPipboyOpenAnim);

		logger::info(" hook sites:");
		for (const auto& site : kHookSites) {
			describe(site.name, site.owner);
			if (HasIDForRuntime(site.owner) && OffsetForRuntime(site) == UNKNOWN_OFFSET &&
				!HasIDForRuntime(site.callsiteTarget)) {
				logger::info(
					"  {:<26} ...but no {} interior offset or callsite target",
					site.name,
					RuntimeFamilyName());
			}
		}
	}
}
