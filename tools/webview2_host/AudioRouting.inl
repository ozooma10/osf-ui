			void TickAudio(View& view)
			{
				view.audio->Tick();
				const auto now = osfui::browser::audio::ClockMs();
				// A completed navigation can recover a failed renderer/capture. Bound
				// retries so an unsupported runtime stays quiet instead of looping.
				if (audioExtensionReady && view.browserLoadSucceeded &&
					view.audio->Status().value("state", "") == "failed" &&
					view.audioRestartAttempts < 2 && now >= view.nextAudioRestart) {
					++view.audioRestartAttempts;
					view.nextAudioRestart = now + 2000;
					view.audio.reset();
					view.audio = std::make_unique<SpatialAudio>(view.webView.Get(),
						[this, id = view.id](const std::string& error) { log.Warn(std::format("view '{}': {}", id, error)); });
					if (!view.audio->Guarded()) { ReportSecurityFailure(view, E_FAIL, "browser audio mute guard failed during recovery"); return; }
					view.audio->Update(audioScene, audioSceneExpires);
					view.audio->SetMuted(browserAudioMuted);
					view.audio->Start(environment.Get(), bootstrapWindow, true);
				}
				const auto status = view.audio->Status(devMode);
				if (status != view.lastAudioState) {
					view.lastAudioState = status;
					if (view.remoteContent) PublishBrowserState(view);
					else if (devMode) {
						if (auto* content = FindView(kBrowserContent)) PublishBrowserState(*content);
					}
				}
			}
			void FailAudioExtension(const std::string& reason)
			{
				audioExtensionFailed = true;
				log.Warn(reason);
				for (auto& view : views) if (view->audio) view->audio->Fail(reason);
			}
			void StartAudioCollectors()
			{
				if (!audioExtensionReady || quit.load()) return;
				for (auto& view : views) if (view->audio) view->audio->Start(environment.Get(), bootstrapWindow, true);
			}
			void InitializeAudio(View& view)
			{
				if (!view.audio) return;
				if (audioExtensionFailed) { view.audio->Fail("Browser audio extension is unavailable"); return; }
				if (audioExtensionReady) { StartAudioCollectors(); return; }
				if (audioExtensionRequested) return;
				audioExtensionRequested = true;
				audioExtensionDeadline = osfui::browser::audio::ClockMs() + 15000;
				ComPtr<ICoreWebView2_13> web13;
				ComPtr<ICoreWebView2Profile> profile;
				ComPtr<ICoreWebView2Profile7> extensions;
				if (audioExtensionPath.empty() || FAILED(view.webView.As(&web13)) ||
					FAILED(web13->get_Profile(&profile)) || FAILED(profile.As(&extensions))) {
					FailAudioExtension("WebView2 extension profile API is unavailable"); return;
				}
				const auto hr = extensions->AddBrowserExtension(audioExtensionPath.c_str(),
					Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
						[this](HRESULT result, ICoreWebView2BrowserExtension* extension) -> HRESULT {
							if (quit.load() || audioExtensionFailed) return S_OK;
							LPWSTR id{};
							BOOL enabled{};
							const bool valid = SUCCEEDED(result) && extension &&
								SUCCEEDED(extension->get_Id(&id)) && id && std::wstring_view(id) == kAudioExtensionId &&
								SUCCEEDED(extension->get_IsEnabled(&enabled)) && enabled;
							::CoTaskMemFree(id);
							if (!valid) { FailAudioExtension("Browser audio extension installation failed or extension is disabled"); return S_OK; }
							audioExtensionReady = true;
							log.InfoFwd("WebView2 world-screen audio extension ready; source output remains native-muted");
							StartAudioCollectors();
							return S_OK;
						}).Get());
				if (FAILED(hr)) FailAudioExtension("Could not install browser audio extension");
			}
			void HandleBrowserAudio(const json& raw)
			{
				if (!remoteMode) return;
				const auto message = msg::FromJson<msg::BrowserAudio>(raw);
				audioScene = message.scene;
				audioSceneExpires = message.expiresMs;
				for (auto& view : views) if (view->audio) view->audio->Update(audioScene, audioSceneExpires);
			}
