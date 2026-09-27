			void SuspendCdpInput(View& a_view)
			{
				if (a_view.cdpInput) a_view.cdpInput->Close();
				a_view.cdpInput.reset();
				a_view.cdpKeys.ReleaseAll();
				a_view.cdpFocused.reset();
				a_view.cdpComposition = false;
			}

			void InitializeCdpInput(View& a_view)
			{
				if (!a_view.webView) return;
				SuspendCdpInput(a_view);
				a_view.cdpInput = std::make_shared<CdpInputQueue>(
					[web = a_view.webView](const std::string& method, const json& params, CdpInputQueue::Completion done) {
						const auto hr = web->CallDevToolsProtocolMethod(ToWide(method).c_str(),
							ToWide(Json::Dump(params)).c_str(),
							Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
								[done](HRESULT result, LPCWSTR) -> HRESULT { done(SUCCEEDED(result)); return S_OK; }).Get());
						if (FAILED(hr)) done(false);
					},
					[this, id = a_view.id] {
						log.Error("CDP input failed, timed out, or exceeded its queue limit for view '" + id + "'");
						if (auto* view = FindView(id)) {
							view->domSeen = false;
							Send(msg::ToJson(msg::LoadEvent{ .view = id, .failed = true,
								.url = ToUtf8(view->currentUrl), .description = "CDP input failed or timed out" }));
						}
					});
				SetCdpFocus(a_view, focusGranted && windowActive && inputTarget == &a_view && !a_view.hidden);
			}

			void ResetCdpKeys(View& a_view)
			{
				const auto releases = a_view.cdpKeys.ReleaseAll();
				if (!a_view.cdpInput) return;
				for (const auto& key : releases) a_view.cdpInput->Push("Input.dispatchKeyEvent", CdpKeyParams(key));
				if (a_view.cdpComposition) {
					a_view.cdpInput->Push("Input.imeSetComposition", json{
						{ "text", "" }, { "selectionStart", 0 }, { "selectionEnd", 0 } });
					a_view.cdpComposition = false;
				}
			}

			void SetCdpFocus(View& a_view, bool a_focused)
			{
				if (!a_view.cdpInput || a_view.cdpFocused == a_focused) return;
				if (!a_focused) ResetCdpKeys(a_view);
				a_view.cdpFocused = a_focused;
				a_view.cdpInput->Push("Emulation.setFocusEmulationEnabled", json{ { "enabled", a_focused } });
			}

			void ReconcileCdpFocus()
			{
				for (auto& view : views) {
					SetCdpFocus(*view, focusGranted && windowActive && inputTarget == view.get() && !view->hidden);
				}
			}

			View* CdpInputTarget()
			{
				if (!focusGranted || !windowActive ||
					!inputTarget || inputTarget->hidden || !inputTarget->cdpInput) return nullptr;
				return inputTarget;
			}

			void HandleKeyboard(const json& a_msg)
			{
				auto* view = CdpInputTarget();
				if (!view) return;
				const auto key = msg::FromJson<msg::Keyboard>(a_msg);
				if (key.vk == 0 || key.vk > 255) return;
				if (++cdpKeyEvents == 1) log.InfoFwd("CDP keyboard input: receiving physical events from Starfield");
				view->cdpKeys.Observe(key);
				view->cdpInput->Push("Input.dispatchKeyEvent", CdpKeyParams(key));
			}

			static void DispatchBackKey(ICoreWebView2* a_web, bool a_down, CdpInputQueue::Completion a_done)
			{
				const msg::Keyboard escape{ .vk = VK_ESCAPE, .down = a_down, .key = "Escape", .code = "Escape" };
				const auto hr = a_web->CallDevToolsProtocolMethod(L"Input.dispatchKeyEvent",
					ToWide(Json::Dump(CdpKeyParams(escape))).c_str(),
					Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
						[web = ComPtr<ICoreWebView2>(a_web), a_down, a_done](HRESULT result, LPCWSTR) -> HRESULT {
							if (SUCCEEDED(result) && a_down) {
								DispatchBackKey(web.Get(), false, a_done);
							} else {
								a_done(SUCCEEDED(result));
							}
							return S_OK;
						}).Get());
				if (FAILED(hr)) a_done(false);
			}

			void HandleBack(const json& a_msg)
			{
				const auto request = msg::FromJson<msg::Back>(a_msg);
				auto* view = CdpInputTarget();
				if (!view || !view->domSeen || view->id != request.view) return;
				const auto input = view->cdpInput;
				const auto epoch = presentationEpoch;
				input->PushTask([this, request, input, epoch, web = view->webView](CdpInputQueue::Completion done) {
					const auto hr = web->ExecuteScript(kHasOpenSelectScript,
						Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
							[this, request, input, epoch, done](HRESULT result, LPCWSTR value) -> HRESULT {
								auto* current = CdpInputTarget();
								// Navigation replaces the input queue. Hide, target changes and
								// focus loss also invalidate a pending browser decision.
								if (quit.load() || !current || !current->domSeen || current->id != request.view || current->cdpInput != input || presentationEpoch != epoch) {
									done(true);
									return S_OK;
								}
								if (FAILED(result) || !value || (std::wstring_view(value) != L"true" && std::wstring_view(value) != L"false")) {
									done(false);
								} else if (std::wstring_view(value) == L"true") {
									// The query and the complete Escape tap are one queued task: later keys cannot overtake the picker dismissal.
									DispatchBackKey(current->webView.Get(), true, done);
								} else {
									Send(msg::ToJson(msg::BackUnhandled{ .view = request.view, .presentationEpoch = request.presentationEpoch }));
									done(true);
								}
								return S_OK;
							}).Get());
					if (FAILED(hr)) done(false);
				});
			}

			void HandleTextInput(const json& a_msg)
			{
				auto* view = CdpInputTarget();
				if (!view) return;
				const auto text = msg::FromJson<msg::TextInput>(a_msg);
				if (++cdpTextEvents == 1) log.InfoFwd("CDP text input: receiving translated text from Starfield");
				if (text.kind == "composition" || text.kind == "cancel") {
					view->cdpComposition = text.kind == "composition";
					view->cdpInput->Push("Input.imeSetComposition", json{
						{ "text", text.text }, { "selectionStart", text.cursor }, { "selectionEnd", text.cursor } });
				} else if (text.kind == "commit") {
					view->cdpComposition = false;
					view->cdpInput->Push("Input.insertText", json{ { "text", text.text } });
				} else if (text.kind == "char" && !text.text.empty()) {
					view->cdpInput->Push("Input.dispatchKeyEvent", json{
						{ "type", "char" }, { "text", text.text }, { "unmodifiedText", text.text } });
				}
			}

			void HandleWindowActive(const json& a_msg)
			{
				windowActive = msg::FromJson<msg::WindowActive>(a_msg).active;
				if (!windowActive) RecoverAllPressedMouseButtons("Starfield focus loss");
				ReconcileCdpFocus();
			}
