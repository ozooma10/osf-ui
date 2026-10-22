			static constexpr std::string_view kPerformancePrefix = "__osfuiPerformance:";

			json PerformanceSnapshot(const View& view, double now)
			{
				auto value = view.performance->Snapshot(now);
				value["view"] = view.id;
				value["width"] = width;
				value["height"] = height;
				return value;
			}

			void StopPerformance(View& view, std::string_view reason)
			{
				if (!view.performance) return;
				auto report = PerformanceSnapshot(view, CapturePerformance::Now());
				report["reason"] = reason;
				log.Info("[OSF UI native performance] " + report.dump());
				view.performance.reset();
			}

			bool HandlePerformanceMessage(View& view, const std::string& text)
			{
				if (!text.starts_with(kPerformancePrefix)) return false;
				if (!devMode || view.remoteContent) return true;
				const auto request = Json::Parse(std::string_view(text).substr(kPerformancePrefix.size()));
				if (!request) return true;
				const auto session = Json::Get(*request, "session", std::string{});
				if (session.empty() || session.size() > 64) return true;
				const auto action = Json::Get(*request, "action", std::string{});
				const auto now = CapturePerformance::Now();
				if (action == "start") {
					if (!view.performance || view.performance->Session() != session) {
						StopPerformance(view, "reset");
						view.performance = std::make_unique<CapturePerformance>(session, now);
					}
				}
				if (!view.performance || view.performance->Session() != session) return true;
				const auto scrollAge = (std::max)(0.0, Json::Get(*request, "scrollAgeMs", 60000.0));
				if (action == "stop") {
					view.performance->Scrolling(Json::Get(*request, "scrolling", false), now, scrollAge);
					StopPerformance(view, "F11 hide");
					return true;
				}
				if (action == "pause") { view.performance->SetActive(false, now); return true; }
				if (action != "start" && action != "sample") return true;
				view.performance->SetActive(!view.hidden && windowActive, now);
				view.performance->Scrolling(Json::Get(*request, "scrolling", false), now, scrollAge);
				const auto reply = json{ { "kind", "osfui.nativePerformance" }, { "session", session },
					{ "payload", PerformanceSnapshot(view, now) } }.dump();
				view.webView->PostWebMessageAsString(ToWide(reply).c_str());
				return true;
			}

			// Zero allocation or clock reads while the developer HUD is disabled.
			void RecordPerformance(unsigned stage)
			{
				if (!devMode) return;
				for (auto& view : views) {
					if (!view->performance) continue;
					const auto now = CapturePerformance::Now();
					if (view->hidden || !windowActive) view->performance->SetActive(false, now);
					if (stage == 0) view->performance->Capture(now);
					else if (stage == 1) view->performance->Published(now);
					else view->performance->Deferred();
				}
			}
