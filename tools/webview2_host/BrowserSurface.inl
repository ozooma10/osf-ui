			PanelRect ViewBounds(const View& view) const
			{
				if (!browserLayout) return {0,0,static_cast<int>(viewportWidth),static_cast<int>(viewportHeight)};
				return BrowserPanelRect(viewportWidth,viewportHeight,browserLayout->chromeHeight,
					browserLayout->fullscreen,browserLayout->landing,
					view.id == browserLayout->chrome ? BrowserPanel::Chrome : BrowserPanel::Content);
			}
			void FocusBrowserView(View* view)
			{
				if (!view || view->hidden || inputTarget == view) return;
				if (inputTarget) ResetCdpKeys(*inputTarget);
				inputTarget=view; ReconcileCdpFocus();
			}
			bool BrowserFocusShortcut(const msg::Keyboard& key)
			{
				if (!browserLayout || !focusGranted || !windowActive ||
					!(key.vk == VK_F6 || (key.vk == 'L' && (key.modifiers & 2)))) return false;
				if (key.down && !key.repeat) {
					if (auto* chrome=FindView(browserLayout->chrome); chrome && !chrome->hidden) {
						FocusBrowserView(chrome);
						const auto event = Json::Dump(json{{"kind","event"},{"name","browser.focusAddress"},{"payload",json::object()}});
						if (chrome->webView) chrome->webView->PostWebMessageAsString(ToWide(event).c_str());
					}
				}
				return true;
			}
			void HandleBrowserLayout(const json& raw)
			{
				auto layout=msg::FromJson<msg::BrowserLayout>(raw);
				auto* chrome=FindView(layout.chrome);
				auto* content=FindView(layout.content);
				if (layout.chrome != kBrowserView || !chrome || chrome->remoteContent ||
					(!layout.content.empty() && (layout.content != kBrowserContent || !content || !content->remoteContent))) {
					FailHost("browser-layout",E_ACCESSDENIED,"Invalid browser pair"); return;
				}
				RecoverAllPressedMouseButtons("browser layout");
				pointerTarget=nullptr;
				browserLayout=layout;
				ApplyViewLayout();
				if (layout.fullscreen || layout.landing) ShowView(*chrome); else HideView(*chrome);
				if (content) { if (layout.landing) HideView(*content); else ShowView(*content); }
				if (!inputTarget || inputTarget->hidden) FocusBrowserView(layout.landing || layout.fullscreen ? chrome : content);
				AdvanceChangedPresentation(layout.presentationEpoch);
			}
			void PublishBrowserState(View& view)
			{
				if (!view.remoteContent || !view.webView) return;
				msg::BrowserState state{.view=view.id,.url=ToUtf8(view.currentUrl),.loading=view.browserLoading};
				LPWSTR value{};
				if (SUCCEEDED(view.webView->get_Source(&value)) && value) state.url=ToUtf8(value);
				::CoTaskMemFree(value); value=nullptr;
				if (SUCCEEDED(view.webView->get_DocumentTitle(&value)) && value) state.title=ToUtf8(value);
				::CoTaskMemFree(value);
				BOOL flag{};
				if (SUCCEEDED(view.webView->get_CanGoBack(&flag))) state.canGoBack=flag;
				if (SUCCEEDED(view.webView->get_CanGoForward(&flag))) state.canGoForward=flag;
				state.secure=view.browserLoadSucceeded && state.url.starts_with("https://");
				ComPtr<ICoreWebView2_8> audio;
				if (SUCCEEDED(view.webView.As(&audio))) {
					if (SUCCEEDED(audio->get_IsDocumentPlayingAudio(&flag))) state.audible=flag;
					if (SUCCEEDED(audio->get_IsMuted(&flag))) state.muted=flag;
				} else state.audibleKnown=false;
				if (view.audio) {
					state.muted = view.audio->Muted();
					if (devMode) state.audio = view.audio->Status();
				}
				const auto encoded=Json::Dump(msg::ToJson(state));
				if (encoded != view.lastBrowserState) { view.lastBrowserState=encoded; Send(msg::ToJson(state)); }
			}
			HRESULT InstallBrowserStateEvents(View& view)
			{
				EventRegistrationToken token{};
				auto changed=[this,target=&view] { PublishBrowserState(*target); };
				auto hr=view.webView->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
					[changed](ICoreWebView2*,ICoreWebView2SourceChangedEventArgs*) -> HRESULT { changed(); return S_OK; }).Get(),&token);
				if (FAILED(hr)) return hr;
				hr=view.webView->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
					[changed](ICoreWebView2*,IUnknown*) -> HRESULT { changed(); return S_OK; }).Get(),&token);
				if (FAILED(hr)) return hr;
				hr=view.webView->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>(
					[changed](ICoreWebView2*,IUnknown*) -> HRESULT { changed(); return S_OK; }).Get(),&token);
				if (FAILED(hr)) return hr;
				ComPtr<ICoreWebView2_8> audio;
				if (FAILED(hr=view.webView.As(&audio))) return hr;
				hr=audio->add_IsDocumentPlayingAudioChanged(Callback<ICoreWebView2IsDocumentPlayingAudioChangedEventHandler>(
					[changed](ICoreWebView2*,IUnknown*) -> HRESULT { changed(); return S_OK; }).Get(),&token);
				if (FAILED(hr)) return hr;
				return audio->add_IsMutedChanged(Callback<ICoreWebView2IsMutedChangedEventHandler>(
					[changed](ICoreWebView2*,IUnknown*) -> HRESULT { changed(); return S_OK; }).Get(),&token);
			}
			void HandleBrowserCommand(const json& raw)
			{
				const auto command=msg::FromJson<msg::BrowserCommand>(raw);
				auto done=[this,token=command.token](HRESULT hr) {
					Send(msg::ToJson(msg::BrowserResult{.token=token,.success=SUCCEEDED(hr),
						.error=SUCCEEDED(hr) ? "" : "Browser command failed"}));
				};
				auto* view=FindView(command.view);
				if (!remoteMode || !view || !view->remoteContent || !view->securityReady || !view->webView) { done(E_ACCESSDENIED); return; }
				auto web=view->webView;
				HRESULT hr=E_INVALIDARG;
				if (command.action == "navigate" && IsAllowedContentDocument(true,ToWide(command.url),{},{}))
					hr=web->Navigate(ToWide(command.url).c_str());
				else if (command.action == "back") hr=web->GoBack();
				else if (command.action == "forward") hr=web->GoForward();
				else if (command.action == "reload") hr=web->Reload();
				else if (command.action == "stop") hr=web->Stop();
				else if (command.action == "mute") {
					if (view->audio) {
						browserAudioMuted = command.value;
						for (auto& source : views) if (source->audio) source->audio->SetMuted(command.value);
						hr=S_OK;
					}
					else {
						ComPtr<ICoreWebView2_8> audio;
						if (SUCCEEDED(hr=web.As(&audio))) hr=audio->put_IsMuted(command.value);
					}
				} else if (command.action == "clearData") {
					ComPtr<ICoreWebView2_13> web13;
					ComPtr<ICoreWebView2Profile> profile;
					ComPtr<ICoreWebView2Profile2> profile2;
					if (SUCCEEDED(hr=web.As(&web13)) && SUCCEEDED(hr=web13->get_Profile(&profile)) &&
						SUCCEEDED(hr=profile.As(&profile2))) {
						hr=profile2->ClearBrowsingDataAll(Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
							[done](HRESULT result) -> HRESULT { done(result); return S_OK; }).Get());
						if (SUCCEEDED(hr)) return;
					}
				}
				done(hr); PublishBrowserState(*view);
			}
			bool HandleBrowserBack(const msg::Back& request)
			{
				if (!browserLayout || request.view != browserLayout->chrome) return false;
				if (!focusGranted || !windowActive || request.presentationEpoch != presentationEpoch) return true;
				auto* content=FindView(browserLayout->content);
				BOOL fullscreen{};
				if (content && content->webView && SUCCEEDED(content->webView->get_ContainsFullScreenElement(&fullscreen)) && fullscreen) {
					FocusBrowserView(content);
					DispatchBackKey(content->webView.Get(),true,[](bool){});
				} else Send(msg::ToJson(msg::BackUnhandled{.view=request.view,.presentationEpoch=request.presentationEpoch}));
				return true;
			}
