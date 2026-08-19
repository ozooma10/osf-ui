
#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Compat/V1/Papyrus.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"
#include "check.h"

namespace
{

	// Count of test-log entries whose text contains a_needle.
	std::size_t LogCount(std::string_view a_needle)
	{
		std::size_t n = 0;
		for (const auto& e : REX::test::Entries()) {
			if (e.find(a_needle) != std::string::npos) {
				++n;
			}
		}
		return n;
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

	// A nameless form that carries an editor id (the best-effort field).
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

	auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();

	API::Papyrus::Install();
	CHECK(vm->natives.contains("SetViewForms"));
	CHECK(vm->natives.contains("GetFormById"));
	CHECK(vm->natives.contains("GetFormsById"));
	CHECK(vm->natives.contains("PushFormsToView"));

	const auto setViewForms =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<RE::TESForm*>)>("SetViewForms");
	const auto setViewStrings =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("SetViewStrings");
	const auto getFormById =
		vm->GetNative<RE::TESForm* (*)(IVM&, std::uint32_t, std::monostate, Str)>("GetFormById");
	const auto getFormsById =
		vm->GetNative<std::vector<RE::TESForm*> (*)(IVM&, std::uint32_t, std::monostate, std::vector<Str>)>("GetFormsById");
	const auto pushForms =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<RE::TESForm*>)>("PushFormsToView");

	std::vector<API::Papyrus::ViewState> drained;
	const auto                           drain = [&] {
		drained = API::Papyrus::TakePendingBatch().states;
	};

	NamedForm    keyword{ 0x0014E8D2, RE::FormType::kKYWD, "Melee Weapons" };
	NamedForm    weapon{ 0x000000FA, RE::FormType::kWEAP, "Eon" };
	EditorIdForm bare{ 0x00000010, RE::FormType::kNONE, "MyEditorId" };

	pushForms(*vm, 0, {}, "T.Forms", "legacy", { &keyword, nullptr, &weapon });
	std::vector<Compat::V1::Papyrus::Push> legacyPushes;
	Compat::V1::Papyrus::DrainPushes([&](const auto& push) { legacyPushes.push_back(push); });
	CHECK(legacyPushes.size() == 1);
	if (!legacyPushes.empty()) {
		CHECK(legacyPushes[0].payload["values"].empty());
		CHECK(legacyPushes[0].payload["forms"].size() == 3);
		CHECK(legacyPushes[0].payload["forms"][0]["formType"] == "KYWD");
		CHECK(legacyPushes[0].payload["forms"][1].is_null());
		CHECK(legacyPushes[0].payload["forms"][2]["name"] == "Eon");
	}

	// --- round-trip: publish -> serialized identity -> echo -> same form ----------
	setViewForms(*vm, 0, {}, "T.Forms", "catalog", { &keyword, &weapon });
	drain();
	CHECK(drained.size() == 1);
	if (drained.size() == 1) {
		const auto& s = drained[0];
		CHECK(s.mod == "t.forms");  // folded to canonical lowercase, like every SetView*
		CHECK(s.key == "catalog");
		CHECK(s.value.is_array());
		CHECK(s.value.size() == 2);
		if (s.value.is_array() && s.value.size() == 2) {
			const auto& kw = s.value[0];
			CHECK(kw.at("formId").get<std::uint32_t>() == 0x0014E8D2u);
			CHECK(kw.at("formType").get<std::string>() == "KYWD");
			CHECK(kw.at("name").get<std::string>() == "Melee Weapons");
			CHECK(!kw.contains("editorId"));  // default GetFormEditorID is empty
			CHECK(s.value[1].at("formType").get<std::string>() == "WEAP");
		}
	}

	CHECK(getFormById(*vm, 0, {}, std::to_string(0x0014E8D2u).c_str()) == &keyword);
	CHECK(getFormById(*vm, 0, {}, "0x0014E8D2") == &keyword);
	CHECK(getFormById(*vm, 0, {}, "0X0014e8d2") == &keyword);

	// --- best-effort fields: editorId when present, numeric-fallback formType -----
	NamedForm unmapped{ 0x00000020, static_cast<RE::FormType>(0xC8), "Oddity" };
	setViewForms(*vm, 0, {}, "t.forms", "misc", { &bare, &unmapped });
	drain();
	CHECK(drained.size() == 1 && drained[0].value.is_array() && drained[0].value.size() == 2);
	if (drained.size() == 1 && drained[0].value.is_array() && drained[0].value.size() == 2) {
		const auto& b = drained[0].value[0];
		CHECK(b.at("editorId").get<std::string>() == "MyEditorId");
		CHECK(!b.contains("name"));  // no TESFullName component
		// A type with no FORM_ENUM_STRING row serializes its numeric value.
		CHECK(drained[0].value[1].at("formType").get<std::string>() == "200");
	}

	setViewForms(*vm, 0, {}, "t.forms", "inv", { &keyword, nullptr, &weapon });
	RE::TESForm::Registry().erase(weapon.GetFormID());  // vanishes pre-drain
	drain();
	CHECK(drained.size() == 1 && drained[0].value.is_array() && drained[0].value.size() == 3);
	if (drained.size() == 1 && drained[0].value.is_array() && drained[0].value.size() == 3) {
		CHECK(!drained[0].value[0].is_null());
		CHECK(drained[0].value[1].is_null());  // None kept its slot
		CHECK(drained[0].value[2].is_null());  // deleted form kept its slot
	}
	CHECK(LogCount("vanished before serialization") == 1);
	RE::TESForm::Registry()[weapon.GetFormID()] = &weapon;  // restore for later sections

	setViewForms(*vm, 0, {}, "t.forms", "catalog", {});
	drain();
	CHECK(drained.size() == 1 && drained[0].value.is_array() && drained[0].value.empty());

	setViewStrings(*vm, 0, {}, "t.forms", "labels", { Str{ "a" } });
	drain();
	CHECK(drained.size() == 1 && drained[0].value.is_array() && drained[0].value.size() == 1);
	if (drained.size() == 1 && drained[0].value.is_array() && drained[0].value.size() == 1) {
		CHECK(drained[0].value[0] == "a");
	}

	// --- shared validation and queue cap with the other SetView* natives ------------
	setViewForms(*vm, 0, {}, "../evil", "k", { &keyword });
	setViewForms(*vm, 0, {}, "t.forms", "", { &keyword });
	drain();
	CHECK(drained.empty());
	CHECK(LogCount("SetViewForms") >= 2);  // both refusals name the native

	for (int i = 0; i < 1024; ++i) {
		setViewStrings(*vm, 0, {}, "t.forms", "k", { Str{ "v" } });
	}
	setViewForms(*vm, 0, {}, "t.forms", "overflow", { &keyword });  // 1025th entry
	CHECK(LogCount("view-state queue full") > 0);
	drain();
	CHECK(drained.size() == 1024);
	for (const auto& s : drained) {
		CHECK(s.key != "overflow");  // the forms value was the one dropped
	}

	// --- resolver parse matrix ------------------------------------------------------
	const auto warnsBefore = LogCount("is not a form id");
	CHECK(getFormById(*vm, 0, {}, "") == nullptr);
	CHECK(getFormById(*vm, 0, {}, "garbage") == nullptr);
	CHECK(getFormById(*vm, 0, {}, "0x") == nullptr);
	CHECK(getFormById(*vm, 0, {}, "123abc") == nullptr);
	CHECK(getFormById(*vm, 0, {}, "-5") == nullptr);
	CHECK(getFormById(*vm, 0, {}, "4294967296") == nullptr);   // > 32 bits
	CHECK(getFormById(*vm, 0, {}, "0x1FFFFFFFF") == nullptr);  // > 32 bits, hex
	CHECK(LogCount("is not a form id") == warnsBefore + 7);

	// Well-formed but absent: the stale-reference case is quiet (DEBUG), not a WARN.
	CHECK(getFormById(*vm, 0, {}, "0x0BADF00D") == nullptr);
	CHECK(LogCount("resolved no form") == 1);
	CHECK(LogCount("is not a form id") == warnsBefore + 7);

	// --- bulk resolver: order + length preserved, unresolved -> None ---------------
	const auto forms = getFormsById(*vm, 0, {},
		{ Str{ "0x0014E8D2" }, Str{ "junk" }, Str{ std::to_string(weapon.GetFormID()) } });
	CHECK(forms.size() == 3);
	if (forms.size() == 3) {
		CHECK(forms[0] == &keyword);
		CHECK(forms[1] == nullptr);
		CHECK(forms[2] == &weapon);
	}

	setViewForms(*vm, 0, {}, "t.forms", "catalog", { &keyword });
	RE::TESLoadGameEvent::GetEventSource()->Notify(RE::TESLoadGameEvent{});
	auto resetBatch = API::Papyrus::TakePendingBatch();
	CHECK(resetBatch.states.empty());                          // queued identities never reach the new session
	CHECK(resetBatch.sessionReset);                            // raised once by the load...
	CHECK(!API::Papyrus::TakePendingBatch().sessionReset);     // ...and consumed by one batch

	std::fprintf(stderr, "papyrus_form_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
