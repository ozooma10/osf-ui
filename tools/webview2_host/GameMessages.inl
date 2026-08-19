			void HandleInit(const json& a_raw)
			{
				if (initialized) return;
				initialized = true;
				const auto a_msg = msg::FromJson<msg::Init>(a_raw);
				gameTopLevel = reinterpret_cast<HWND>(
					static_cast<std::uintptr_t>(a_msg.topLevelHwnd));
				viewsRoot = std::filesystem::path(ToWide(a_msg.viewsPath));
				virtualHost = ToWide(a_msg.virtualHost);
				width = (std::max)(1u, a_msg.width);
				height = (std::max)(1u, a_msg.height);
				userData = std::filesystem::path(ToWide(a_msg.userDataDir));
				devMode = a_msg.devMode;
				highRefreshCapture = a_msg.highRefreshCapture;
				defaultHidden = a_msg.hidden;
				if (userData.empty()) {
					log.Error("init without userDataDir");
					return;
				}
				std::optional<LUID> requestedAdapter;
				if (a_raw.contains("adapterLuidLow") && a_raw.contains("adapterLuidHigh")) {
					LUID luid{};
					luid.LowPart = a_msg.adapterLuidLow;
					luid.HighPart = static_cast<LONG>(a_msg.adapterLuidHigh);
					requestedAdapter = luid;
				}
				if (!InitializeGraphics(requestedAdapter)) {
					byeReason = "graphics-init-failed";
					quit.store(true);
					return;
				}
				rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
				log.Info(std::format("init: views='{}' {}x{} hidden={} highRefreshCapture={} topLevel=0x{:X}", ToUtf8(viewsRoot.native()), width, height, defaultHidden, highRefreshCapture, reinterpret_cast<std::uintptr_t>(gameTopLevel)));
				BeginEnvironment();
			}

			void HandleNavigate(const json& a_raw)
			{
				const auto a_msg = msg::FromJson<msg::Navigate>(a_raw);
				if (a_msg.id.empty()) {
					log.Warn("navigate without id ignored");
					return;
				}
				auto* view = FindView(a_msg.id);
				if (!view) view = &CreateView(a_msg.id);
				view->logicalHeight = (std::max)(1u, a_msg.logicalHeight);
				ApplyScale(*view);
				std::string entry = a_msg.entry;
				if (a_msg.legacyApi) {
					entry = OSFUI::Compat::V1::WithLegacyApiQuery(entry);
				}
				std::string path = a_msg.id + "/" + entry;
				std::ranges::replace(path, '\\', '/');
				view->pendingNavigate = L"https://" + virtualHost + L"/" + ToWide(path);
				if (view->webView) DrainQueuedViewWork(*view);
				else RequestController(*view);
			}

			void HandleResize(const json& a_raw)
			{
				const auto a_msg = msg::FromJson<msg::Resize>(a_raw);
				ApplyResize(a_msg.width, a_msg.height);
			}

			void HandleSetHidden(const json& a_raw)
			{
				auto* view = ResolveView(a_raw);
				if (!view) return;
				const auto a_msg = msg::FromJson<msg::SetHidden>(a_raw);
				if (a_msg.hidden) {
					HideView(*view);
				} else {
					view->pendingPresentationEpoch = a_msg.presentationEpoch;
					ShowView(*view);
				}
			}

			void HandleSetOrder(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg)) {
					view->order = msg::FromJson<msg::SetOrder>(a_msg).order;
					ReorderVisuals();
				}
			}

			void HandleSetInputTarget(const json& a_msg)
			{
				auto* view = ResolveView(a_msg);
				if (!view) return;
				if (inputTarget && inputTarget != view) inputTarget->nativePopupOpen = false;
				inputTarget = view;
				log.Info(std::format("input-target view -> '{}'", view->id));
				if (focusGranted && view->controller && !view->hidden) {
					view->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				}
				ReconcileInputWidgetSubclass();
				ApplyMouseCapture();
			}

			void HandleFocus(const json& a_msg)
			{
				focusGranted = msg::FromJson<msg::Focus>(a_msg).focused;
				if (!focusGranted) {
					for (auto& view : views) view->nativePopupOpen = false;
				}
				SetRawMouseInput(focusGranted);
				if (focusGranted && inputTarget && inputTarget->controller && !inputTarget->hidden) {
					inputTarget->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				}
				ReconcileInputWidgetSubclass();
				ApplyMouseCapture();
				ApplyCaptureCadence();
			}

			void HandleMouse(const json& a_msg) { SendMouse(a_msg); }

			void HandleKey(const json& a_raw)
			{
				if (!inputTarget || !inputTarget->webView) return;
				const auto a_msg = msg::FromJson<msg::Key>(a_raw);
				const auto payload = Json::Dump(json{ { "__osfuiKey", {
					{ "vk", a_msg.vk },
					{ "down", a_msg.down },
				} } });
				inputTarget->webView->PostWebMessageAsJson(ToWide(payload).c_str());
			}

			void HandleFrameAck(const json& a_msg)
			{
				const auto serial = msg::FromJson<msg::FrameAck>(a_msg).serial;
				auto current = ackedSerial.load();
				while (serial > current &&
					!ackedSerial.compare_exchange_weak(current, serial)) {}
			}

			void HandlePostWeb(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg)) {
					view->queuedPostWeb.push_back(msg::FromJson<msg::PostWeb>(a_msg).json);
					DrainQueuedViewWork(*view);
				}
			}

			void HandleOpenDevTools(const json& a_msg)
			{
				if (!devMode) {
					log.Warn("openDevTools ignored outside devMode");
					return;
				}
				handledKeys.erase(VK_F12);
				if (auto* view = ResolveView(a_msg); view && view->webView) {
					const auto hr = view->webView->OpenDevToolsWindow();
					if (FAILED(hr)) {
						log.Warn(std::format(
							"view '{}': OpenDevToolsWindow failed (0x{:08X})",
							view->id, static_cast<unsigned>(hr)));
					}
				}
			}

			void HandleAccelState(const json& a_msg)
			{
				const bool wasCaptured = captured;
				const auto state = msg::FromJson<msg::AccelState>(a_msg);
				toggleScan = state.toggleScan;
				captured = state.captured;
				captureArmed = state.captureArmed;
				captureUpScan = state.captureUpScan;
				ReconcileInputWidgetSubclass();
				if (wasCaptured && !captured) handledKeys.clear();
			}

			void HandleDestroyView(const json& a_msg)
			{
				auto* view = ResolveView(a_msg);
				if (!view) return;
				log.Info(std::format("destroying view '{}'", view->id));
				egressWarned.erase(view->id);
				DestroyOneView(*view);
				const bool wasInputTarget = view == inputTarget;
				std::erase_if(views, [view](const std::unique_ptr<View>& a_view) {
					return a_view.get() == view;
				});
				RefreshCaptureVisibility();
				if (wasInputTarget) inputTarget = views.empty() ? nullptr : views.front().get();
				if (!AnyRevealPending()) ApplyDeferredHides();
			}

			void HandleShutdown(const json&)
			{
				log.Info(std::format(
					"shutdown requested by the game (accelEvents={}, frames={})",
					accelEvents, frameSerial));
				quit.store(true);
			}

			void HandleGameMessage(const json& a_msg)
			{
				using Handler = void (App::*)(const json&);
				static constexpr std::pair<std::string_view, Handler> handlers[]{
					{ msg::Init::kType, &App::HandleInit },
					{ msg::Navigate::kType, &App::HandleNavigate },
					{ msg::Resize::kType, &App::HandleResize },
					{ msg::SetHidden::kType, &App::HandleSetHidden },
					{ msg::SetOrder::kType, &App::HandleSetOrder },
					// SetInputTarget's kType is the `setActive` compatibility spelling.
					{ msg::SetInputTarget::kType, &App::HandleSetInputTarget },
					{ msg::Focus::kType, &App::HandleFocus },
					{ msg::Mouse::kType, &App::HandleMouse },
					{ msg::Key::kType, &App::HandleKey },
					{ msg::FrameAck::kType, &App::HandleFrameAck },
					{ msg::PostWeb::kType, &App::HandlePostWeb },
					{ msg::OpenDevTools::kType, &App::HandleOpenDevTools },
					{ msg::AccelState::kType, &App::HandleAccelState },
					{ msg::DestroyView::kType, &App::HandleDestroyView },
					{ msg::Shutdown::kType, &App::HandleShutdown },
				};
				const auto type = Json::Get(a_msg, "type", "");
				for (const auto& [name, handler] : handlers) {
					if (type == name) {
						(this->*handler)(a_msg);
						return;
					}
				}
				log.Warn("unknown message type '" + type + "' ignored");
			}
