#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "RE/B/BSScriptUtil.h"

namespace OSFUI::API::Papyrus
{
	// Only the frozen v1 Papyrus adapter may use these registry and form-serializer hooks.
	std::int32_t RegisterLegacyActionInstance(
		const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
		const RE::BSFixedString& a_fn, const RE::BSFixedString& a_mod,
		bool a_wantsArgs, const char* a_native);
	std::int32_t RegisterLegacyActionStatic(const RE::BSFixedString& a_script,
		const RE::BSFixedString& a_fn, const RE::BSFixedString& a_mod,
		bool a_wantsArgs, const char* a_native);
	nlohmann::json SerializeFormForLegacyPush(std::uint32_t a_formId);
}

namespace OSFUI::Compat::V1::Papyrus
{
	struct Push
	{
		std::string mod;
		nlohmann::json payload;
	};

	void BindNatives(RE::BSScript::IVirtualMachine* a_vm);
	void QueueStringPush(std::string a_mod, std::string a_key,
		std::vector<std::string> a_values);
	void QueueFormPush(std::string a_mod, std::string a_key,
		std::vector<std::uint32_t> a_formIds);
	void DrainPushes(const std::function<void(const Push&)>& a_deliver);
	void ClearPendingPushes();
}
