			void HandleInit(const json& a_msg)
			{
				if (initialized) return;
				initialized = true;
				gameTopLevel = reinterpret_cast<HWND>(
					static_cast<std::uintptr_t>(a_msg.value("topLevelHwnd", 0ull)));
				viewsRoot = std::filesystem::path(ToWide(a_msg.value("viewsPath", "")));
				virtualHost = ToWide(a_msg.value("virtualHost", "osfui.local"));
				width = (std::max)(1u, a_msg.value("width", 1u));
				height = (std::max)(1u, a_msg.value("height", 1u));
				userData = std::filesystem::path(ToWide(a_msg.value("userDataDir", "")));
				devMode = a_msg.value("devMode", false);
				defaultHidden = a_msg.value("hidden", true);
				if (userData.empty()) {
					log.Error("init without userDataDir");
					return;
				}
				std::optional<LUID> requestedAdapter;
				if (a_msg.contains("adapterLuidLow") && a_msg.contains("adapterLuidHigh")) {
					LUID luid{};
					luid.LowPart = a_msg.value("adapterLuidLow", 0u);
					luid.HighPart = static_cast<LONG>(a_msg.value("adapterLuidHigh", 0u));
					requestedAdapter = luid;
				}
				if (!InitializeGraphics(requestedAdapter)) {
					byeReason = "graphics-init-failed";
					quit.store(true);
					return;
				}
				rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
				log.Info(std::format("init: views='{}' {}x{} hidden={} topLevel=0x{:X}",
					ToUtf8(viewsRoot.native()), width, height, defaultHidden,
					reinterpret_cast<std::uintptr_t>(gameTopLevel)));
				BeginEnvironment();
			}

			void HandleNavigate(const json& a_msg)
			{
				const std::string id = a_msg.value("id", "");
				if (id.empty()) {
					log.Warn("navigate without id ignored");
					return;
				}
				auto* view = FindView(id);
				if (!view) view = &CreateView(id);
				NoteViewActivity(*view, /*a_clearSuspendRequest=*/true);
				view->bridge = a_msg.value("bridge", true);
				view->logicalHeight = (std::max)(1u,
					a_msg.value("logicalHeight", kDefaultLogicalHeight));
				ApplyScale(*view);
				std::string path = id + "/" + a_msg.value("entry", "index.html");
				std::ranges::replace(path, '\\', '/');
				view->pendingNavigate = L"https://" + virtualHost + L"/" + ToWide(path);
				if (view->webView) DrainQueuedViewWork(*view);
				else RequestController(*view);
			}

			void HandleResize(const json& a_msg)
			{
				ApplyResize(a_msg.value("width", 1u), a_msg.value("height", 1u));
			}

			void HandlePrewarm(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg)) {
					view->prewarm = true;
					BeginPrewarm(*view);
				}
			}

			void HandleSuspendView(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg); view && view->hidden) {
					view->suspendRequested = true;
					view->nextSuspendAttemptMs = ::GetTickCount64();
				}
			}

			void HandleSetHidden(const json& a_msg)
			{
				auto* view = ResolveView(a_msg);
				if (!view) return;
				if (a_msg.value("hidden", true)) {
					HideView(*view);
				} else {
					view->pendingPresentationEpoch = a_msg.value("presentationEpoch", 0ull);
					ShowView(*view);
				}
			}

			void HandleSetOrder(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg)) {
					view->order = a_msg.value("order", 0);
					ReorderVisuals();
				}
			}

			void HandleSetActive(const json& a_msg)
			{
				auto* view = ResolveView(a_msg);
				if (!view) return;
				if (active && active != view) active->nativePopupOpen = false;
				active = view;
				log.Info(std::format("active view -> '{}'", view->id));
				if (focusGranted && view->controller && !view->hidden) {
					view->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				}
				ReconcileInputWidgetSubclass();
				ApplyMouseCapture();
			}

			void HandleFocus(const json& a_msg)
			{
				focusGranted = a_msg.value("focused", false);
				if (!focusGranted) {
					for (auto& view : views) view->nativePopupOpen = false;
				}
				SetRawMouseInput(focusGranted);
				if (focusGranted && active && active->controller && !active->hidden) {
					active->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
				}
				ReconcileInputWidgetSubclass();
				ApplyMouseCapture();
				ApplyCaptureCadence();
			}

			void HandleMouse(const json& a_msg) { SendMouse(a_msg); }

			void HandleKey(const json& a_msg)
			{
				if (!active || !active->webView) return;
				const auto payload = json{ { "__osfuiKey", {
					{ "vk", a_msg.value("vk", 0u) },
					{ "down", a_msg.value("down", false) },
				} } }.dump();
				active->webView->PostWebMessageAsJson(ToWide(payload).c_str());
			}

			void HandleFrameAck(const json& a_msg)
			{
				const auto serial = a_msg.value("serial", 0ull);
				auto current = ackedSerial.load();
				while (serial > current &&
					!ackedSerial.compare_exchange_weak(current, serial)) {}
			}

			void HandlePostWeb(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg)) {
					view->queuedPostWeb.push_back(a_msg.value("json", ""));
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
					NoteViewActivity(*view, /*a_clearSuspendRequest=*/false);
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
				toggleScan = a_msg.value("toggleScan", 0u);
				captured = a_msg.value("captured", false);
				captureArmed = a_msg.value("captureArmed", false);
				captureUpScan = a_msg.value("captureUpScan", 0u);
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
				const bool wasActive = view == active;
				std::erase_if(views, [view](const std::unique_ptr<View>& a_view) {
					return a_view.get() == view;
				});
				if (wasActive) active = views.empty() ? nullptr : views.front().get();
				if (!AnyRevealPending()) ApplyDeferredHides();
			}

			void HandleShutdown(const json&)
			{
				log.Info(std::format(
					"shutdown requested by the game (accelEvents={}, frames={})",
					accelEvents, frameSerial));
				quit.store(true);
			}

			void HandleCommand(const json& a_msg)
			{
				using Handler = void (App::*)(const json&);
				static constexpr std::pair<std::string_view, Handler> handlers[]{
					{ "init", &App::HandleInit },
					{ "navigate", &App::HandleNavigate },
					{ "resize", &App::HandleResize },
					{ "prewarm", &App::HandlePrewarm },
					{ "suspendView", &App::HandleSuspendView },
					{ "setHidden", &App::HandleSetHidden },
					{ "setOrder", &App::HandleSetOrder },
					{ "setActive", &App::HandleSetActive },
					{ "focus", &App::HandleFocus },
					{ "mouse", &App::HandleMouse },
					{ "key", &App::HandleKey },
					{ "frameAck", &App::HandleFrameAck },
					{ "postWeb", &App::HandlePostWeb },
					{ "openDevTools", &App::HandleOpenDevTools },
					{ "accelState", &App::HandleAccelState },
					{ "destroyView", &App::HandleDestroyView },
					{ "shutdown", &App::HandleShutdown },
				};
				const auto type = a_msg.value("type", std::string{});
				for (const auto& [name, handler] : handlers) {
					if (type == name) {
						(this->*handler)(a_msg);
						return;
					}
				}
				log.Warn("unknown message type '" + type + "' ignored");
			}
