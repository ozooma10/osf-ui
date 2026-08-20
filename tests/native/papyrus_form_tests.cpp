#include "API/PapyrusApi.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"
#include "check.h"

namespace
{
	std::size_t LogCount(std::string_view a_needle)
	{
		return std::ranges::count_if(REX::test::Entries(), [&](const auto& a_entry) {
			return a_entry.find(a_needle) != std::string::npos;
		});
	}

	struct NamedForm :
		RE::TESForm,
		RE::TESFullName
	{
		NamedForm(std::uint32_t a_id, RE::FormType a_type, std::string a_name)
		{
			formID = a_id;
			formType = a_type;
			fullName = std::move(a_name);
			Registry()[a_id] = this;
		}
	};

	struct EditorIdForm : RE::TESForm
	{
		EditorIdForm(std::uint32_t a_id, RE::FormType a_type, std::string a_editorId) :
			editorId(std::move(a_editorId))
		{
			formID = a_id;
			formType = a_type;
			Registry()[a_id] = this;
		}

		const char* GetFormEditorID() const override { return editorId.c_str(); }
		std::string editorId;
	};
}

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DebugEnabled() { return true; }
	void SetDebugLogging(bool) {}
}

int main()
{
	using namespace OSFUI;
	using IVM = RE::BSScript::IVirtualMachine;
	using Str = RE::BSFixedString;
	using Var = RE::BSScript::Variable;

	auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
	API::Papyrus::Install();
	CHECK(vm->natives.contains("SetState"));
	CHECK(vm->natives.contains("SetStateForms"));
	CHECK(vm->natives.contains("ReplyForms"));
	CHECK(vm->natives.contains("EmitEvent"));
	CHECK(!vm->natives.contains("GetFormById"));
	CHECK(!vm->natives.contains("GetFormsById"));

	const auto setState = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, const Var*)>("SetState");
	const auto setStateForms = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, std::vector<RE::TESForm*>)>("SetStateForms");
	const auto emitEvent = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, Str, std::optional<std::vector<const Var*>>)>("EmitEvent");

	NamedForm keyword{ 0x0014E8D2, RE::FormType::kKYWD, "Melee Weapons" };
	NamedForm weapon{ 0x000000FA, RE::FormType::kWEAP, "Eon" };
	EditorIdForm bare{ 0x00000010, RE::FormType::kNONE, "MyEditorId" };
	NamedForm unmapped{ 0x00000020, static_cast<RE::FormType>(0xC8), "Oddity" };

	// Typed Form[] state preserves ordering and None slots; scalar Var carries a
	// Form without exposing a separate resolver API.
	CHECK(setStateForms(*vm, 0, {}, "T.Forms", "catalog", { &keyword, nullptr, &weapon }));
	Var scalarForm;
	RE::BSScript::PackVariable(scalarForm, &keyword);
	CHECK(setState(*vm, 0, {}, "T.Forms", "selected", &scalarForm));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.states.size() == 2);
		if (batch.states.size() == 2) {
			const auto& forms = batch.states[0].value;
			CHECK(batch.states[0].mod == "t.forms" && batch.states[0].key == "catalog");
			CHECK(forms.is_array() && forms.size() == 3);
			if (forms.is_array() && forms.size() == 3) {
				CHECK(forms[0]["formId"] == 0x0014E8D2u);
				CHECK(forms[0]["formType"] == "KYWD");
				CHECK(forms[0]["name"] == "Melee Weapons");
				CHECK(forms[1].is_null());
				CHECK(forms[2]["formType"] == "WEAP");
			}
			CHECK(batch.states[1].key == "selected");
			CHECK(batch.states[1].value["formId"] == keyword.GetFormID());
		}
	}

	CHECK(setStateForms(*vm, 0, {}, "t.forms", "metadata", { &bare, &unmapped }));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.states.size() == 1);
		if (batch.states.size() == 1 && batch.states[0].value.size() == 2) {
			CHECK(batch.states[0].value[0]["editorId"] == "MyEditorId");
			CHECK(!batch.states[0].value[0].contains("name"));
			CHECK(batch.states[0].value[1]["formType"] == "200");
		}
	}

	CHECK(setStateForms(*vm, 0, {}, "t.forms", "vanishing", { &keyword, &weapon }));
	RE::TESForm::Registry().erase(weapon.GetFormID());
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.states.size() == 1 && batch.states[0].value.size() == 2);
		if (batch.states.size() == 1 && batch.states[0].value.size() == 2) {
			CHECK(!batch.states[0].value[0].is_null());
			CHECK(batch.states[0].value[1].is_null());
		}
	}
	CHECK(LogCount("vanished before serialization") == 1);
	RE::TESForm::Registry()[weapon.GetFormID()] = &weapon;

	// None is a legal scalar; arbitrary ScriptObjects are not bridge values.
	Var none;
	CHECK(setState(*vm, 0, {}, "t.forms", "none", &none));
	auto badType = std::make_shared<RE::BSScript::ObjectTypeInfo>();
	badType->name = Str{ "NotAForm" };
	auto badObject = std::make_shared<RE::BSScript::Object>();
	badObject->type = RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo>(std::move(badType));
	Var unsupported;
	unsupported = RE::BSTSmartPointer<RE::BSScript::Object>(std::move(badObject));
	CHECK(!setState(*vm, 0, {}, "t.forms", "bad", &unsupported));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.states.size() == 1 && batch.states[0].value.is_null());
	}

	// Var[] events serialize mixed scalar/Form arguments on the main thread.
	Var label;
	label = Str{ "weapon" };
	CHECK(emitEvent(*vm, 0, {}, "T.Forms", "selected",
		std::vector<const Var*>{ &label, &scalarForm, &none }));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.events.size() == 1);
		if (batch.events.size() == 1) {
			CHECK(batch.events[0].args.size() == 3);
			CHECK(batch.events[0].args[0] == "weapon");
			CHECK(batch.events[0].args[1]["formId"] == keyword.GetFormID());
			CHECK(batch.events[0].args[2].is_null());
		}
	}

	// JavaScript SerializedForm arguments become real Form Vars in the callback.
	const auto registerSend = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t,
		std::monostate, Str, Str, Str)>("RegisterSendStatic");
	CHECK(registerSend(*vm, 0, {}, "FormSink", "T.Forms", "inspect") != 0);
	vm->calls.clear();
	CHECK(API::Papyrus::OnViewSend("t.forms", "inspect",
		{ API::Papyrus::FormValue{ keyword.GetFormID() },
			API::Papyrus::FormValue{ 0x0BADF00D } }, "other.mod/view"));
	CHECK(vm->calls.size() == 1);
	if (vm->calls.size() == 1) {
		CHECK(vm->calls[0].fn == "OnOSFUISend");
		CHECK((vm->calls[0].argTypes ==
			std::vector<std::string>{ "string", "object", "none", "string" }));
		CHECK(vm->calls[0].args.back() == "other.mod/view");
	}

	// Scalar and typed-array Form replies share the same request ledger.
	const auto registerRequest = vm->GetNative<std::int32_t (*)(IVM&, std::uint32_t,
		std::monostate, Str, Str, Str)>("RegisterRequestStatic");
	const auto reply = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, const Var*)>("Reply");
	const auto replyForms = vm->GetNative<bool (*)(IVM&, std::uint32_t, std::monostate,
		Str, std::vector<RE::TESForm*>)>("ReplyForms");
	CHECK(registerRequest(*vm, 0, {}, "FormSink", "T.Forms", "choose") != 0);

	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("t.forms", "choose", {}, "caller/view", "defer-form"));
	CHECK(vm->calls.size() == 1);
	std::string token = vm->calls.empty() ? "" : vm->calls[0].args.back();
	CHECK(reply(*vm, 0, {}, token, &scalarForm));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.replies.size() == 1);
		CHECK(batch.replies.size() == 1 && batch.replies[0].value["formId"] == keyword.GetFormID());
	}

	vm->calls.clear();
	CHECK(API::Papyrus::OnViewRequest("t.forms", "choose", {}, "caller/view", "defer-forms"));
	token = vm->calls.empty() ? "" : vm->calls[0].args.back();
	CHECK(replyForms(*vm, 0, {}, token, { &keyword, nullptr, &weapon }));
	{
		auto batch = API::Papyrus::TakePendingBatch();
		CHECK(batch.replies.size() == 1 && batch.replies[0].value.size() == 3);
		if (batch.replies.size() == 1 && batch.replies[0].value.size() == 3) {
			CHECK(batch.replies[0].value[0]["formType"] == "KYWD");
			CHECK(batch.replies[0].value[1].is_null());
			CHECK(batch.replies[0].value[2]["name"] == "Eon");
		}
	}

	// A load drops queued form identities before they can leak into a new save.
	CHECK(setStateForms(*vm, 0, {}, "t.forms", "stale", { &keyword }));
	RE::TESLoadGameEvent::GetEventSource()->Notify(RE::TESLoadGameEvent{});
	const auto resetBatch = API::Papyrus::TakePendingBatch();
	CHECK(resetBatch.states.empty());
	CHECK(resetBatch.events.empty());
	CHECK(resetBatch.replies.empty());
	CHECK(resetBatch.sessionReset);

	std::fprintf(stderr, "papyrus_form_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
