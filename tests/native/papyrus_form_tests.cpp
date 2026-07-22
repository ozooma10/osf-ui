// Host-side tests for form references across the bridge (protocol 1.3,
// docs/form-references-design.md): the REAL api/PapyrusApi.cpp compiled
// against stubs/RE (recording VM + a TESForm test registry), driven through
// the same natives the game binds. Covers PushFormsToView's capture-ids/
// serialize-at-drain split (identity fields, null-slot preservation, empty
// pushes, shared validation and cap) and the GetFormById/GetFormsById
// resolvers (decimal + hex parse matrix, stale references).
// Assert-style; process exit code is the failure count.

#include "api/BridgeApi.h"
#include "api/PapyrusApi.h"

#include "RE/B/BSScriptUtil.h"
#include "RE/E/Events.h"
#include "RE/T/TESForm.h"
#include "RE/T/TESFullName.h"

namespace
{
	int g_failures = 0;
	int g_checks = 0;

#define CHECK(expr)                                                                     \
	do {                                                                                \
		++g_checks;                                                                     \
		if (!(expr)) {                                                                  \
			++g_failures;                                                               \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
		}                                                                               \
	} while (0)

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

	// A named form, the way real game forms carry the component: TESFullName
	// multiple-inherited next to TESForm, found via starfield_cast.
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

// core/Log.h declarations (real impl pulls game deps — stub, as in
// papyrus_action_tests.cpp; SettingsStore references these).
namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DevMode() { return true; }
	void SetDevMode(bool) {}
}

int main()
{
	using namespace OSFUI;
	using IVM = RE::BSScript::IVirtualMachine;
	using Str = RE::BSFixedString;

	auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();

	API::Papyrus::Install();
	CHECK(vm->natives.contains("PushFormsToView"));
	CHECK(vm->natives.contains("GetFormById"));
	CHECK(vm->natives.contains("GetFormsById"));

	const auto pushForms =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<RE::TESForm*>)>("PushFormsToView");
	const auto pushToView =
		vm->GetNative<void (*)(IVM&, std::uint32_t, std::monostate, Str, Str, std::vector<Str>)>("PushToView");
	const auto getFormById =
		vm->GetNative<RE::TESForm* (*)(IVM&, std::uint32_t, std::monostate, Str)>("GetFormById");
	const auto getFormsById =
		vm->GetNative<std::vector<RE::TESForm*> (*)(IVM&, std::uint32_t, std::monostate, std::vector<Str>)>("GetFormsById");

	std::vector<API::Papyrus::ViewPush> drained;
	const auto drain = [&] {
		drained.clear();
		API::Papyrus::DrainViewPushes([&](const API::Papyrus::ViewPush& a_push) { drained.push_back(a_push); });
	};

	NamedForm    keyword{ 0x0014E8D2, RE::FormType::kKYWD, "Melee Weapons" };
	NamedForm    weapon{ 0x000000FA, RE::FormType::kWEAP, "Eon" };
	EditorIdForm bare{ 0x00000010, RE::FormType::kNONE, "MyEditorId" };

	// --- round-trip: push -> serialized identity -> echo -> same form -------------
	pushForms(*vm, 0, {}, "T.Forms", "catalog", { &keyword, &weapon });
	drain();
	CHECK(drained.size() == 1);
	if (drained.size() == 1) {
		const auto& p = drained[0];
		CHECK(p.mod == "t.forms");  // folded to canonical lowercase, like PushToView
		CHECK(p.key == "catalog");
		CHECK(p.values.empty());
		CHECK(p.forms.has_value());
		if (p.forms) {
			CHECK(p.forms->is_array());
			CHECK(p.forms->size() == 2);
			const auto& kw = (*p.forms)[0];
			CHECK(kw.at("formId").get<std::uint32_t>() == 0x0014E8D2u);
			CHECK(kw.at("formType").get<std::string>() == "KYWD");
			CHECK(kw.at("name").get<std::string>() == "Melee Weapons");
			CHECK(!kw.contains("editorId"));  // default GetFormEditorID is empty
			CHECK((*p.forms)[1].at("formType").get<std::string>() == "WEAP");
		}
	}

	// The echo path: what a view sends back resolves to the same form. Decimal
	// is what the host's number->string arg coercion produces; both hex
	// spellings are accepted for authors quoting a display id.
	CHECK(getFormById(*vm, 0, {}, std::to_string(0x0014E8D2u).c_str()) == &keyword);
	CHECK(getFormById(*vm, 0, {}, "0x0014E8D2") == &keyword);
	CHECK(getFormById(*vm, 0, {}, "0X0014e8d2") == &keyword);

	// --- best-effort fields: editorId when present, numeric-fallback formType -----
	NamedForm unmapped{ 0x00000020, static_cast<RE::FormType>(0xC8), "Oddity" };
	pushForms(*vm, 0, {}, "t.forms", "misc", { &bare, &unmapped });
	drain();
	CHECK(drained.size() == 1 && drained[0].forms && drained[0].forms->size() == 2);
	if (drained.size() == 1 && drained[0].forms && drained[0].forms->size() == 2) {
		const auto& b = (*drained[0].forms)[0];
		CHECK(b.at("editorId").get<std::string>() == "MyEditorId");
		CHECK(!b.contains("name"));  // no TESFullName component
		// A type with no FORM_ENUM_STRING row serializes its numeric value.
		CHECK((*drained[0].forms)[1].at("formType").get<std::string>() == "200");
	}

	// --- null slots: None inputs and forms deleted between queue and drain --------
	pushForms(*vm, 0, {}, "t.forms", "inv", { &keyword, nullptr, &weapon });
	RE::TESForm::Registry().erase(weapon.GetFormID());  // vanishes pre-drain
	drain();
	CHECK(drained.size() == 1 && drained[0].forms && drained[0].forms->size() == 3);
	if (drained.size() == 1 && drained[0].forms && drained[0].forms->size() == 3) {
		CHECK(!(*drained[0].forms)[0].is_null());
		CHECK((*drained[0].forms)[1].is_null());  // None kept its slot
		CHECK((*drained[0].forms)[2].is_null());  // deleted form kept its slot
	}
	CHECK(LogCount("vanished before serialization") == 1);
	RE::TESForm::Registry()[weapon.GetFormID()] = &weapon;  // restore for later sections

	// --- empty forms push still delivers; a plain PushToView has no forms ---------
	pushForms(*vm, 0, {}, "t.forms", "catalog", {});
	drain();
	CHECK(drained.size() == 1 && drained[0].forms.has_value() && drained[0].forms->empty());

	pushToView(*vm, 0, {}, "t.forms", "labels", { Str{ "a" } });
	drain();
	CHECK(drained.size() == 1 && !drained[0].forms.has_value());

	// --- shared validation and queue cap with PushToView ---------------------------
	pushForms(*vm, 0, {}, "notdotted", "k", { &keyword });
	pushForms(*vm, 0, {}, "t.forms", "", { &keyword });
	drain();
	CHECK(drained.empty());
	CHECK(LogCount("PushFormsToView") >= 2);  // both refusals name the native

	for (int i = 0; i < 1024; ++i) {
		pushToView(*vm, 0, {}, "t.forms", "k", { Str{ "v" } });
	}
	pushForms(*vm, 0, {}, "t.forms", "overflow", { &keyword });  // 1025th entry
	CHECK(LogCount("view-push queue full") > 0);
	drain();
	CHECK(drained.size() == 1024);
	for (const auto& p : drained) {
		CHECK(p.key != "overflow");  // the forms push was the one dropped
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

	std::fprintf(stderr, "papyrus_form_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures;
}
