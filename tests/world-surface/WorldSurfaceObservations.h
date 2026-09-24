#pragma once

#include "Core/Paths.h"
#include "OSFUI_Views.h"
#include "World/WorldTexture.h"

namespace OSFUI::Testing
{
	inline void OpenWorldFixtureOverlay(const std::vector<WorldTexture::SurfaceStats>& a_stats)
	{
		// This header is included only by OSFUI_TEST_HARNESS builds. Wait for
		// actual loaded gameplay instead of opening the HUD during Main Menu.
		static bool attempted = false;
		if (attempted)
			return;
		const auto fixture = std::ranges::find_if(a_stats, [](const auto& stat) {
			return stat.id == "osfui-world-test/screen" && stat.completedFrames > 2 && !stat.gpuFailed;
		});
		if (fixture == a_stats.end())
			return;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* ui = RE::UI::GetSingleton();
		if (!player || !player->parentCell || player->parentCell->GetFormID() != 0x002BE3A9 || !ui ||
			ui->IsMenuOpen(RE::BSFixedString("MainMenu")) || ui->IsMenuOpen(RE::BSFixedString("LoadingMenu")))
			return;
		std::error_code error;
		if (!std::filesystem::exists(Paths::ViewsDir() / "osfui-world-test" / "overlay" / "manifest.json", error)) {
			attempted = true;  // World-only fixture: leave presentation untouched.
			return;
		}
		auto* views = API::Views::RequestInterface();
		if (!views)
			return;
		attempted = true;
		// RequestMenu admits only views in the discovered catalog and queues the
		// ordinary presentation path; no test code touches renderer/input state.
		if (views->RequestMenu("osfui-world-test/overlay", true)) {
			REX::INFO("WorldSurface test: passive HUD requested after loaded QASmoke frames");
		} else {
			REX::ERROR("WorldSurface test: passive HUD was not admitted from the discovered view catalog");
		}
	}

	inline void LogWorldSurfaceObservations(const std::vector<WorldTexture::SurfaceStats>& a_stats)
	{
		OpenWorldFixtureOverlay(a_stats);
		static std::string evictionRequest;
		std::ifstream      requestFile(Paths::ViewsDir() / "osfui-world-test" / "evict.request");
		std::string        request;
		if (requestFile && std::getline(requestFile, request) && !request.empty() && request != evictionRequest) {
			evictionRequest = request;
			for (std::size_t i = 0; i < a_stats.size(); ++i) WorldTexture::RequestEviction(i);
			REX::INFO("WorldSurface eviction requested {}", request);
		}
		const auto cache = WorldTexture::SnapshotCache();
		REX::INFO("WorldSurface cache {}", nlohmann::json({ { "budgetBytes", cache.budgetBytes }, { "residentBytes", cache.residentBytes },
															  { "retiredOutputs", cache.retiredOutputs }, { "allocationDeferrals", cache.allocationDeferrals } })
											   .dump());
		// Exercise the public native-to-web route independently for each board.
		// Only the multi-screen fixture includes screen-2; ordinary views never
		// receive these fictional prices, and release builds exclude this file.
		if (std::ranges::any_of(a_stats, [](const auto& stat) { return stat.id == "osfui-world-test/screen-2"; })) {
			static std::uint64_t sequence = 0;
			++sequence;
			if (auto* views = API::Views::RequestInterface()) {
				constexpr const char* ids[]{ "osfui-world-test/screen", "osfui-world-test/screen-2", "z-world-test/screen", "z-world-test/screen-2" };
				constexpr const char* symbols[]{ "NOVA", "DEIM", "STRO", "RYUJ" };
				for (std::size_t i = 0; i < 4; ++i) {
					const auto payload = nlohmann::json{ { "symbol", symbols[i] }, { "price", 100.0 * (i + 1) + (sequence * (i + 1) % 37) * 0.25 } }.dump();
					(void)views->SendToWeb(ids[i], "stock.update", payload.c_str());
				}
			}
		}
		auto snapshot = nlohmann::json::array();
		for (const auto& stat : a_stats) snapshot.push_back({ { "id", stat.id }, { "boundDescriptors", stat.boundDescriptors },
			{ "texture", stat.texture },
			{ "outputGeneration", stat.outputGeneration }, { "engineOwners", stat.engineOwners },
			{ "residentBytes", stat.residentBytes }, { "allocationDeferrals", stat.allocationDeferrals }, { "evictionPending", stat.evictionPending }, { "width", stat.width }, { "height", stat.height },
			{ "submittedFrames", stat.submittedFrames }, { "copiedFrames", stat.copiedFrames },
			{ "lastCopiedFrame", stat.lastCopiedFrame }, { "completedFrames", stat.completedFrames },
			{ "lastCompletedFrame", stat.lastCompletedFrame }, { "ringGeneration", stat.ringGeneration },
			{ "rejectedFrames", stat.rejectedFrames }, { "ringOpenFailures", stat.ringOpenFailures },
			{ "producerDisconnects", stat.producerDisconnects },
			{ "gpuFailed", stat.gpuFailed }, { "skippedBusy", stat.skippedBusy } });
		REX::INFO("WorldSurface snapshot {}", snapshot.dump());
	}
}
