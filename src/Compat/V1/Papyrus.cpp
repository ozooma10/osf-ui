#include "Compat/V1/Papyrus.h"

#include "API/PapyrusApi.h"
#include "Core/StringUtil.h"
#include "Core/Ids.h"

#include "RE/T/TESForm.h"

namespace OSFUI::Compat::V1::Papyrus
{
	namespace
	{
		using PapVM = RE::BSScript::IVirtualMachine;
		constexpr std::string_view kScriptName = API::Papyrus::kPlatformScriptName;
		constexpr std::size_t kMaxPendingPushes = 1024;

		struct QueuedPush
		{
			std::string mod;
			std::string key;
			std::vector<std::string> values;
			std::vector<std::uint32_t> formIds;
			bool forms{ false };
		};

		struct State
		{
			std::mutex lock;
			std::vector<QueuedPush> pushes;
		};

		State& GetState()
		{
			static State* const state = new State;
			return *state;
		}

		std::optional<std::pair<std::string, std::string>> FoldTarget(
			std::string a_mod, std::string a_key, std::string_view a_native)
		{
			a_mod = StringUtil::ToLowerAscii(a_mod);
			if (!Ids::IsValidModId(a_mod) || a_key.empty()) {
				REX::WARN("PapyrusApi: [content] legacy {}('{}', '{}') refused (invalid mod id or empty key)",
					a_native, a_mod.substr(0, 64), a_key.substr(0, 64));
				return std::nullopt;
			}
			return std::pair{ std::move(a_mod), std::move(a_key) };
		}

		void Enqueue(QueuedPush a_push)
		{
			std::lock_guard lock(GetState().lock);
			if (GetState().pushes.size() >= kMaxPendingPushes) {
				REX::WARN("PapyrusApi: legacy push queue full; dropping '{}.{}'", a_push.mod, a_push.key);
				return;
			}
			GetState().pushes.push_back(std::move(a_push));
		}

		std::int32_t RegisterForViewActions(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn,
			RE::BSFixedString a_mod)
		{
			return API::Papyrus::RegisterLegacyActionInstance(
				a_receiver, a_fn, a_mod, false, "RegisterForViewActions");
		}

		std::int32_t RegisterForViewActionsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_mod)
		{
			return API::Papyrus::RegisterLegacyActionStatic(
				a_script, a_fn, a_mod, false, "RegisterForViewActionsStatic");
		}

		std::int32_t RegisterForViewActionsArgs(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn,
			RE::BSFixedString a_mod)
		{
			return API::Papyrus::RegisterLegacyActionInstance(
				a_receiver, a_fn, a_mod, true, "RegisterForViewActionsArgs");
		}

		std::int32_t RegisterForViewActionsArgsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_mod)
		{
			return API::Papyrus::RegisterLegacyActionStatic(
				a_script, a_fn, a_mod, true, "RegisterForViewActionsArgsStatic");
		}

		void PushToView(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod,
			RE::BSFixedString a_key, std::vector<RE::BSFixedString> a_values)
		{
			std::vector<std::string> values;
			values.reserve(a_values.size());
			for (const auto& value : a_values) values.emplace_back(value.c_str());
			QueueStringPush(a_mod.c_str(), a_key.c_str(), std::move(values));
		}

		void PushFormsToView(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod,
			RE::BSFixedString a_key, std::vector<RE::TESForm*> a_forms)
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) {
				ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0u);
			}
			QueueFormPush(a_mod.c_str(), a_key.c_str(), std::move(ids));
		}
	}

	void BindNatives(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->BindNativeMethod(kScriptName, "RegisterForViewActions", &RegisterForViewActions, true, false);
		a_vm->BindNativeMethod(kScriptName, "RegisterForViewActionsStatic", &RegisterForViewActionsStatic, true, false);
		a_vm->BindNativeMethod(kScriptName, "RegisterForViewActionsArgs", &RegisterForViewActionsArgs, true, false);
		a_vm->BindNativeMethod(kScriptName, "RegisterForViewActionsArgsStatic", &RegisterForViewActionsArgsStatic, true, false);
		a_vm->BindNativeMethod(kScriptName, "PushToView", &PushToView, true, false);
		a_vm->BindNativeMethod(kScriptName, "PushFormsToView", &PushFormsToView, true, false);
	}

	void QueueStringPush(std::string a_mod, std::string a_key, std::vector<std::string> a_values)
	{
		auto target = FoldTarget(std::move(a_mod), std::move(a_key), "PushToView");
		if (!target) return;
		Enqueue(QueuedPush{ std::move(target->first), std::move(target->second),
			std::move(a_values), {}, false });
	}

	void QueueFormPush(std::string a_mod, std::string a_key, std::vector<std::uint32_t> a_formIds)
	{
		auto target = FoldTarget(std::move(a_mod), std::move(a_key), "PushFormsToView");
		if (!target) return;
		Enqueue(QueuedPush{ std::move(target->first), std::move(target->second),
			{}, std::move(a_formIds), true });
	}

	void DrainPushes(const std::function<void(const Push&)>& a_deliver)
	{
		std::vector<QueuedPush> pending;
		{
			std::lock_guard lock(GetState().lock);
			pending.swap(GetState().pushes);
		}
		for (auto& queued : pending) {
			nlohmann::json payload{
				{ "mod", queued.mod },
				{ "key", queued.key },
				{ "values", queued.values },
			};
			if (queued.forms) {
				payload["forms"] = nlohmann::json::array();
				for (const auto id : queued.formIds) {
					payload["forms"].push_back(API::Papyrus::SerializeFormForLegacyPush(id));
				}
			}
			a_deliver(Push{ std::move(queued.mod), std::move(payload) });
		}
	}

	void ClearPendingPushes()
	{
		std::lock_guard lock(GetState().lock);
		GetState().pushes.clear();
	}

}
