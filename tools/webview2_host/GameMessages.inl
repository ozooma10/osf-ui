			void HandleInit(const json& a_raw)
			{
				if (initialized) return;
				initialized = true;
				const auto a_msg = msg::FromJson<msg::Init>(a_raw);
				gameTopLevel = reinterpret_cast<HWND>(
					static_cast<std::uintptr_t>(a_msg.topLevelHwnd));
				viewsRoot = std::filesystem::path(ToWide(a_msg.viewsPath));
				width = (std::max)(1u, a_msg.width);
				height = (std::max)(1u, a_msg.height);
				viewportWidth = width;
				viewportHeight = height;
				userData = std::filesystem::path(ToWide(a_msg.userDataDir));
				devMode = a_msg.devMode;
				language = a_msg.language;
				windowActive = GameIsForeground();
				log.InfoFwd("input mode: forwarded CDP; native focus remains in Starfield");
				if (userData.empty()) {
					FailHost("init", E_INVALIDARG, "init without userDataDir");
					return;
				}
				const auto leasePath = viewsRoot / OSFUI::ViewCache::kUseLock;
				std::error_code leaseEc;
				if (std::filesystem::exists(leasePath, leaseEc) && !viewsLease.Open(leasePath)) {
					log.Error(std::format("could not acquire views-cache lease '{}' ({})", ToUtf8(leasePath.native()), ::GetLastError()));
					byeReason = "views-cache-lease-failed";
					quit.store(true);
					return;
				}
				if (leaseEc) {
					log.Error("could not inspect views-cache lease: " + leaseEc.message());
					byeReason = "views-cache-lease-failed";
					quit.store(true);
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
				log.Info(std::format("init: views='{}' {}x{} language='{}' topLevel=0x{:X}", ToUtf8(viewsRoot.native()), width, height, language, reinterpret_cast<std::uintptr_t>(gameTopLevel)));
				BeginEnvironment();
			}

			void HandleNavigate(const json& a_raw)
			{
				const auto a_msg = msg::FromJson<msg::Navigate>(a_raw);
				if (!OSFUI::Ids::IsValidQualifiedViewId(a_msg.id)) {
					log.Warn(std::format("navigate with invalid qualified id '{}' ignored", a_msg.id));
					return;
				}
				auto* view = FindView(a_msg.id);
				if (!view) view = &CreateView(a_msg.id);
				view->logicalHeight = (std::max)(1u, a_msg.logicalHeight);
				ApplyScale(*view);
				std::string entry = a_msg.entry;
				std::ranges::replace(entry, '\\', '/');
				view->pendingNavigate = L"https://" + std::wstring(kViewHost) + L"/" + view->modId + L"/" + view->viewName + L"/" + ToWide(entry);
				if (view->webView) DrainQueuedViewWork(*view);
				else RequestController(*view);
			}

			void HandleResize(const json& a_raw)
			{
				const auto a_msg = msg::FromJson<msg::Resize>(a_raw);
				ApplyResize(a_msg.width, a_msg.height);
			}

			void HandleViewport(const json& a_raw)
			{
				const auto a_msg = msg::FromJson<msg::Viewport>(a_raw);
				ApplyViewport(a_msg.width, a_msg.height, a_msg.presentationEpoch);
			}

			void HandlePointerInput(const json& a_raw)
			{
				const bool enabled = msg::FromJson<msg::PointerInput>(a_raw).enabled;
				if (pointerInputEnabled == enabled) return;
				if (!enabled) {
					RecoverAllPressedMouseButtons("geometry transition");
				}
				pointerInputEnabled = enabled;
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
				if (inputTarget && inputTarget != view) {
					RecoverPressedMouseButtons(*inputTarget, "input target change");
				}
				inputTarget = view;
				ReconcileCdpFocus();
				log.Info(std::format("input-target view -> '{}'", view->id));
			}

			void HandleFocus(const json& a_msg)
			{
				const auto request = msg::FromJson<msg::Focus>(a_msg);
				if (request.epoch < focusEpoch) {
					log.Info(std::format("stale focus request ignored (epoch {} < {})",
						request.epoch, focusEpoch));
					return;
				}
				focusEpoch = request.epoch;
				log.Info(std::format("focus request begin: focused={} epoch={}", request.focused, focusEpoch));
				if (!request.focused) RecoverAllPressedMouseButtons("focus revoke");
				focusGranted = request.focused;
				windowActive = GameIsForeground();
				if (!request.view.empty()) {
					if (auto* requestedView = FindView(request.view)) {
						if (inputTarget && inputTarget != requestedView) {
							RecoverPressedMouseButtons(*inputTarget, "focus target change");
						}
						inputTarget = requestedView;
					}
				}
				ReconcileCdpFocus();
				log.Info(std::format("focus request complete: focused={} epoch={}", focusGranted, focusEpoch));
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
				const auto ack = msg::FromJson<msg::FrameAck>(a_msg);
				if (ack.slot >= kRingSlots) {
					log.Warn(std::format("ignoring frame acknowledgement for invalid slot {}", ack.slot));
					return;
				}
				auto& completed = ackedSerials[ack.slot];
				auto current = completed.load();
				while (ack.serial > current &&
					!completed.compare_exchange_weak(current, ack.serial)) {}
				RetryPendingCapture();
				RepublishLatest(true);
			}

			void HandlePostWeb(const json& a_msg)
			{
				if (auto* view = ResolveView(a_msg)) {
					constexpr std::size_t kMaxQueuedPostWeb = 64;
					constexpr std::size_t kMaxQueuedPostWebBytes = 8u * 1024u * 1024u;
					auto payload = msg::FromJson<msg::PostWeb>(a_msg).json;
					if (payload.size() > kMaxQueuedPostWebBytes) return;
					while (view->queuedPostWeb.size() >= kMaxQueuedPostWeb ||
						view->queuedPostWebBytes + payload.size() > kMaxQueuedPostWebBytes) {
						view->queuedPostWebBytes -= view->queuedPostWeb.front().size();
						view->queuedPostWeb.pop_front();
						if (!view->queuedPostWebOverflowWarned) {
							view->queuedPostWebOverflowWarned = true;
							log.Warn(std::format("view '{}': pending web messages exceeded the {}-message/{}-byte cap; dropping oldest", view->id, kMaxQueuedPostWeb, kMaxQueuedPostWebBytes));
						}
					}
					view->queuedPostWebBytes += payload.size();
					view->queuedPostWeb.push_back(std::move(payload));
					DrainQueuedViewWork(*view);
				}
			}

			void HandleOpenDevTools(const json& a_msg)
			{
				if (!devMode) {
					log.Warn("openDevTools ignored outside devMode");
					return;
				}
				if (auto* view = ResolveView(a_msg); view && view->webView) {
					const auto hr = view->webView->OpenDevToolsWindow();
					if (FAILED(hr)) {
						log.Warn(std::format(
							"view '{}': OpenDevToolsWindow failed (0x{:08X})",
							view->id, static_cast<unsigned>(hr)));
					}
				}
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
				ReconcileCdpFocus();
				if (!AnyRevealPending()) ApplyDeferredHides();
			}

			void HandleGameMessage(const json& a_msg)
			{
				using Handler = void (App::*)(const json&);
				static constexpr std::pair<std::string_view, Handler> handlers[]{
					{ msg::Init::kType, &App::HandleInit },
					{ msg::Navigate::kType, &App::HandleNavigate },
					{ msg::Resize::kType, &App::HandleResize },
					{ msg::Viewport::kType, &App::HandleViewport },
					{ msg::PointerInput::kType, &App::HandlePointerInput },
					{ msg::SetHidden::kType, &App::HandleSetHidden },
					{ msg::SetOrder::kType, &App::HandleSetOrder },
					{ msg::SetInputTarget::kType, &App::HandleSetInputTarget },
					{ msg::Focus::kType, &App::HandleFocus },
					{ msg::Mouse::kType, &App::HandleMouse },
					{ msg::Key::kType, &App::HandleKey },
					{ msg::Keyboard::kType, &App::HandleKeyboard },
					{ msg::TextInput::kType, &App::HandleTextInput },
					{ msg::WindowActive::kType, &App::HandleWindowActive },
					{ msg::FrameAck::kType, &App::HandleFrameAck },
					{ msg::PostWeb::kType, &App::HandlePostWeb },
					{ msg::OpenDevTools::kType, &App::HandleOpenDevTools },
					{ msg::DestroyView::kType, &App::HandleDestroyView },
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
