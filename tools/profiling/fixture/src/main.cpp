#include <SFSE/SFSE.h>
#include <SFSE/Logger.h>
#include <RE/Starfield.h>
#include <REX/FModule.h>

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "CarbonUI_BenchmarkAPI.h"
#include "OSFUI_API.h"

namespace
{
	using Json = nlohmann::json;

	constexpr const char* kViewId = "uibench/shootout";
	constexpr const char* kEndpoint = "uibench.report";

	OSFUI::API::Client g_osfui;
	bool g_osfuiRegistered{};
	const CarbonUI::IAPI* g_carbon{};
	CarbonUI::ViewHandle g_carbonView{ CarbonUI::kInvalidView };
	std::uint32_t g_carbonStartAttempts{};
	bool g_carbonStartupPending{};
	bool g_presentationRequested{};
	bool g_fixtureVisible{};
	std::mutex g_telemetryMutex;
	std::filesystem::path g_telemetryPath;

	struct PendingTelemetry
	{
		std::string line;
	};

	struct Config
	{
		std::string scenario{ "static" };
		std::string fixtureHash{ "unconfigured" };
		std::uint32_t width{ 1920 };
		std::uint32_t height{ 1080 };
	};

	Config ReadConfig() noexcept
	{
		Config result;
		try {
			const std::filesystem::path game = REX::FModule::GetExecutingModule().GetFileName();
			const auto path = game.parent_path() / "Data" / "SFSE" / "Plugins" /
				"UIBench" / "config.json";
			std::ifstream input(path);
			if (!input) {
				return result;
			}
			const auto value = Json::parse(input, nullptr, false);
			if (value.is_discarded() || !value.is_object()) {
				return result;
			}
			result.scenario = value.value("scenario", result.scenario);
			result.fixtureHash = value.value("fixtureHash", result.fixtureHash);
			result.width = value.value("width", result.width);
			result.height = value.value("height", result.height);
		} catch (...) {}
		return result;
	}

	void CALLBACK WriteTelemetry(PTP_CALLBACK_INSTANCE, void* a_context) noexcept
	{
		std::unique_ptr<PendingTelemetry> pending{ static_cast<PendingTelemetry*>(a_context) };
		try {
			std::scoped_lock lock(g_telemetryMutex);
			if (!pending || g_telemetryPath.empty()) {
				return;
			}
			std::ofstream output(g_telemetryPath, std::ios::app);
			if (output) {
				output << pending->line << '\n';
			}
		} catch (...) {}
	}

	void AppendTelemetry(Json a_payload, const char* a_provider) noexcept
	{
		try {
			if (!a_payload.is_object()) {
				a_payload = Json{ { "invalidPayload", true }, { "payload", std::move(a_payload) } };
			}
			a_payload["provider"] = a_provider;
			a_payload["gamePid"] = ::GetCurrentProcessId();
			a_payload["receivedUnixMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count();

			auto pending = std::make_unique<PendingTelemetry>();
			pending->line = a_payload.dump();
			if (::TrySubmitThreadpoolCallback(&WriteTelemetry, pending.get(), nullptr)) {
				(void)pending.release();
			}
		} catch (...) {}
	}

	void OnOSFReport(const char*, const char* a_payloadJson, const char*, void*) noexcept
	{
		try {
			const auto payload = Json::parse(a_payloadJson ? a_payloadJson : "null", nullptr, false);
			AppendTelemetry(payload, "OSFUI");
		} catch (...) {}
	}

	const char* OnCarbonReport(const char* a_argsJson, void*) noexcept
	{
		try {
			const auto args = Json::parse(a_argsJson ? a_argsJson : "[]", nullptr, false);
			if (args.is_array() && !args.empty() && args[0].is_string()) {
				AppendTelemetry(Json::parse(args[0].get<std::string>(), nullptr, false), "CarbonUI");
			} else {
				AppendTelemetry(args, "CarbonUI");
			}
		} catch (...) {}
		return "true";
	}

	std::string CarbonURL(const Config& a_config)
	{
		// Set-UIBenchFixture.ps1 constrains these values to URL-safe characters.
		return "file:///Data/SFSE/Plugins/UIBench/index.html?scenario=" + a_config.scenario +
			"&width=" + std::to_string(a_config.width) +
			"&height=" + std::to_string(a_config.height) +
			"&fixtureHash=" + a_config.fixtureHash;
	}

	bool StartOSFUI() noexcept
	{
		if (!g_osfui.Init()) {
			return false;
		}
		g_osfui.RegisterSend(kEndpoint, &OnOSFReport, nullptr);
		if (!g_osfui.RegisterView(kViewId)) {
			return false;
		}
		g_osfuiRegistered = true;
		return true;
	}

	bool StartCarbonUI(const SFSE::MessagingInterface* a_messaging) noexcept
	{
		if (!g_carbon) {
			g_carbon = CarbonUI::RequestCarbonUIAPI(a_messaging);
		}
		if (!g_carbon || g_carbon->GetVersion(g_carbon->ctx) < CarbonUI::kAPIVersion) {
			return false;
		}
		if (g_carbonView != CarbonUI::kInvalidView &&
			g_carbon->IsViewValid(g_carbon->ctx, g_carbonView)) {
			return true;
		}
		const auto config = ReadConfig();
		const auto url = CarbonURL(config);
		CarbonUI::ViewDesc desc{};
		desc.structSize = sizeof(desc);
		desc.url = url.c_str();
		desc.width = 0;
		desc.height = 0;
		desc.z = 1000;
		desc.transparent = 0;
		desc.visible = 0;
		desc.focusable = 0;
		desc.ownerName = "UIBench";
		g_carbonView = g_carbon->CreateView(g_carbon->ctx, &desc);
		if (g_carbonView == CarbonUI::kInvalidView) {
			return false;
		}
		if (!g_carbon->RegisterListener(g_carbon->ctx, g_carbonView, kEndpoint,
			&OnCarbonReport, nullptr)) {
			g_carbon->DestroyView(g_carbon->ctx, g_carbonView);
			g_carbonView = CarbonUI::kInvalidView;
			return false;
		}
		return true;
	}

	void PresentFixture() noexcept
	{
		g_presentationRequested = true;
		if (g_fixtureVisible) {
			return;
		}
		if (g_osfuiRegistered) {
			if (g_osfui.RequestMenu(kViewId, true)) {
				g_fixtureVisible = true;
				REX::INFO("UIBench: loading menu closed; OSF UI fixture open request queued");
			} else {
				REX::WARN("UIBench: loading menu closed, but OSF UI rejected the fixture open request");
			}
			return;
		}
		if (g_carbon && g_carbonView != CarbonUI::kInvalidView) {
			const bool shown = g_carbon->ShowView(g_carbon->ctx, g_carbonView, true);
			if (shown) {
				g_fixtureVisible = true;
				REX::INFO("UIBench: loading menu closed; passive Carbon UI fixture shown");
			} else {
				REX::WARN("UIBench: loading menu closed, but Carbon UI rejected fixture presentation");
			}
			return;
		}
		REX::INFO("UIBench: loading menu closed; no-framework baseline ready");
	}

	void HideFixtureForSystemMenu() noexcept
	{
		g_presentationRequested = false;
		g_fixtureVisible = false;
		if (g_osfuiRegistered) {
			(void)g_osfui.RequestMenu(kViewId, false);
		}
		if (g_carbon && g_carbonView != CarbonUI::kInvalidView) {
			(void)g_carbon->ShowView(g_carbon->ctx, g_carbonView, false);
		}
	}

	class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink instance;
			return &instance;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			const std::string_view name{ a_event.menuName };
			if (a_event.opening && (name == "LoadingMenu" || name == "MainMenu")) {
				HideFixtureForSystemMenu();
			} else if (!a_event.opening && name == "LoadingMenu") {
				PresentFixture();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void FixtureTick() noexcept
	{
		if (!g_carbonStartupPending) {
			return;
		}
		if (StartCarbonUI(SFSE::GetMessagingInterface())) {
			g_carbonStartupPending = false;
			REX::INFO("UIBench: Carbon UI fixture created hidden after framework initialization");
			if (g_presentationRequested) {
				PresentFixture();
			}
			return;
		}
		constexpr std::uint32_t kMaximumAttempts = 600;
		if (++g_carbonStartAttempts >= kMaximumAttempts) {
			g_carbonStartupPending = false;
			REX::WARN("UIBench: Carbon UI view manager did not become ready after {} game-frame attempts",
				kMaximumAttempts);
		}
	}

	void InitializeAfterDataLoad() noexcept
	{
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->RegisterSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
			REX::INFO("UIBench: waiting for LoadingMenu to close before presenting the fixture");
		} else {
			REX::WARN("UIBench: UI singleton is unavailable; fixture cannot auto-open after loading");
		}
		if (g_osfuiRegistered) {
			return;
		}
		g_carbon = CarbonUI::RequestCarbonUIAPI(SFSE::GetMessagingInterface());
		if (!g_carbon) {
			REX::INFO("UIBench: no supported UI framework detected; fixture remains inert");
			return;
		}
		g_carbonStartupPending = true;
		if (auto* tasks = SFSE::GetTaskInterface()) {
			tasks->AddPermanentTask(FixtureTick);
		} else {
			g_carbonStartupPending = false;
			REX::WARN("UIBench: SFSE task interface unavailable; Carbon UI startup retry was not installed");
		}
	}

	void OnSFSEMessage(SFSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}
		if (a_message->type == SFSE::MessagingInterface::kPostLoad) {
			if (const auto logDir = SFSE::log::log_directory()) {
				g_telemetryPath = *logDir / "UIBench.telemetry.jsonl";
			}
			(void)StartOSFUI();
			return;
		}
		if (a_message->type == SFSE::MessagingInterface::kPostDataLoad) {
			if (auto* tasks = SFSE::GetTaskInterface()) {
				tasks->AddTask(InitializeAfterDataLoad);
			} else {
				REX::WARN("UIBench: SFSE task interface unavailable; post-load initialization was not queued");
			}
		}
	}
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	SFSE::Init(a_sfse);
	auto* messaging = SFSE::GetMessagingInterface();
	if (!messaging) {
		return false;
	}
	messaging->RegisterListener(OnSFSEMessage);
	return true;
}
