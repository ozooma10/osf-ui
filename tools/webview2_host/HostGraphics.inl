			bool InitializeGraphics(const std::optional<LUID>& a_requestedLuid)
			{
				ComPtr<IDXGIAdapter1> selectedAdapter;
				if (a_requestedLuid) {
					ComPtr<IDXGIFactory1> factory;
					const auto factoryHr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
					if (SUCCEEDED(factoryHr)) {
						for (UINT index = 0; ; ++index) {
							ComPtr<IDXGIAdapter1> candidate;
							if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
							DXGI_ADAPTER_DESC1 desc{};
							if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
								desc.AdapterLuid.LowPart == a_requestedLuid->LowPart &&
								desc.AdapterLuid.HighPart == a_requestedLuid->HighPart) {
								selectedAdapter = std::move(candidate);
								break;
							}
						}
					} else {
						log.Warn(std::format(
							"CreateDXGIFactory1 failed while matching the game adapter (0x{:08X}); "
							"falling back to the browser-host default GPU", static_cast<unsigned>(factoryHr)));
					}
					if (!selectedAdapter) {
						log.Warn(std::format(
							"game adapter LUID 0x{:08X}:0x{:08X} was not found in the browser host; "
							"falling back to the browser-host default GPU",
							static_cast<std::uint32_t>(a_requestedLuid->HighPart),
							a_requestedLuid->LowPart));
					}
				}

				const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
				D3D_FEATURE_LEVEL actual{};
				auto hr = ::D3D11CreateDevice(selectedAdapter.Get(),
					selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
					nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
					static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
					&device, &actual, &context);
				if (FAILED(hr)) {
					log.Error(std::format("D3D11CreateDevice failed (0x{:08X})", static_cast<unsigned>(hr)));
					return false;
				}
				if (FAILED(device.As(&device5)) || FAILED(context.As(&context4))) {
					log.Error("ID3D11Device5/DeviceContext4 unavailable (need Win10 1703+) — no shared-fence transport");
					return false;
				}
				ComPtr<IDXGIDevice> dxgi;
				if (FAILED(device.As(&dxgi))) return false;
				ComPtr<IDXGIAdapter> actualAdapter;
				DXGI_ADAPTER_DESC actualDesc{};
				if (FAILED(dxgi->GetAdapter(&actualAdapter)) ||
					FAILED(actualAdapter->GetDesc(&actualDesc))) {
					log.Error("could not identify the D3D11 capture adapter");
					return false;
				}
				graphicsAdapterLuid = actualDesc.AdapterLuid;
				log.InfoFwd(std::format("D3D11 capture adapter '{}' LUID 0x{:08X}:0x{:08X}",
					ToUtf8(actualDesc.Description),
					static_cast<std::uint32_t>(graphicsAdapterLuid.HighPart),
					graphicsAdapterLuid.LowPart));

				winrt::com_ptr<::IInspectable> inspectable;
				hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put());
				if (FAILED(hr)) {
					log.Error(std::format("CreateDirect3D11DeviceFromDXGIDevice failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				captureDevice = inspectable.as<
					winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
				return true;
			}

			bool InitializeComposition()
			{
				DispatcherQueueOptions dq{ sizeof(DispatcherQueueOptions),
					DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };
				const auto hr = ::CreateDispatcherQueueController(dq,
					reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
						winrt::put_abi(dispatcher)));
				if (FAILED(hr)) {
					log.Error(std::format("CreateDispatcherQueueController failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				compositor = winrt::Windows::UI::Composition::Compositor();
				rootVisual = compositor.CreateContainerVisual();
				rootVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
				rootVisual.IsVisible(true);
				return true;
			}

			bool CreateWindows()
			{
				bootstrapWindow = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
					L"OSFUI WebView2 Browser Host Bootstrap", WS_POPUP,
					-32000, -32000, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
				if (!bootstrapWindow) {
					log.Error(std::format("bootstrap HWND creation failed ({})", ::GetLastError()));
					return false;
				}
				::ShowWindow(bootstrapWindow, SW_SHOWNOACTIVATE);
				hostWindow = ::CreateWindowExW(0, L"STATIC", L"OSFUI WebView2 Browser Host",
					WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, bootstrapWindow, nullptr,
					::GetModuleHandleW(nullptr), nullptr);
				if (!hostWindow) {
					log.Error(std::format("browser-host child HWND creation failed ({})", ::GetLastError()));
					return false;
				}
				return true;
			}

			void ReleaseRing()
			{
				for (auto& slot : ring) {
					if (slot.localHandle) {
						::CloseHandle(slot.localHandle);
						slot.localHandle = nullptr;
					}
					slot.texture.Reset();
					slot.lastSerial = 0;
				}
				ringWidth = ringHeight = 0;
				ringWrite = 0;
			}

			bool EnsureProduceFence()
			{
				if (!produceFence) {
					const auto hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
						IID_PPV_ARGS(&produceFence));
					if (FAILED(hr)) {
						log.Error(std::format("CreateFence(produce) failed (0x{:08X})",
							static_cast<unsigned>(hr)));
						return false;
					}
				}
				return true;
			}

			[[nodiscard]] HANDLE DuplicateToGame(HANDLE a_local)
			{
				HANDLE remote = nullptr;
				if (!::DuplicateHandle(::GetCurrentProcess(), a_local, gameProcess,
						&remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
					log.Error(std::format("DuplicateHandle into game failed ({})", ::GetLastError()));
					return nullptr;
				}
				return remote;
			}

			// Host STA. Returns false when the ring could not be built.
			bool EnsureRing(std::uint32_t a_width, std::uint32_t a_height)
			{
				if (ring[0].texture && ringWidth == a_width && ringHeight == a_height) {
					return true;
				}
				ReleaseRing();
				if (!EnsureProduceFence()) return false;

				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = a_width;
				desc.Height = a_height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
				// No keyed-mutex fallback: the game reads slots without AcquireSync, so a keyed-mutex ring would be read unsynchronized. Fail cleanly instead.
				if (const auto hr = device->CreateTexture2D(&desc, nullptr, &ring[0].texture); FAILED(hr)) {
					log.Error(std::format("shared texture creation failed (0x{:08X})", static_cast<unsigned>(hr)));
					return false;
				}
				for (std::uint32_t i = 1; i < kRingSlots; ++i) {
					if (FAILED(device->CreateTexture2D(&desc, nullptr, &ring[i].texture))) {
						log.Error("shared texture ring creation failed");
						ReleaseRing();
						return false;
					}
				}
				for (auto& slot : ring) {
					ComPtr<IDXGIResource1> resource;
					if (FAILED(slot.texture.As(&resource)) || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &slot.localHandle))) {
						log.Error("CreateSharedHandle failed");
						ReleaseRing();
						return false;
					}
				}
				ringWidth = a_width;
				ringHeight = a_height;
				ringWrite = 0;

				// Duplicate everything into the game and announce the new ring.
				std::vector<std::uint64_t> slots;
				slots.reserve(kRingSlots);
				std::vector<HANDLE> remoteHandles;
				remoteHandles.reserve(kRingSlots + 1);
				const auto closeUnannouncedHandles = [&] {
					for (const auto remote : remoteHandles) {
						HANDLE local = nullptr;
						if (::DuplicateHandle(gameProcess, remote, ::GetCurrentProcess(), &local,
							0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
							::CloseHandle(local);
						}
					}
				};
				for (auto& slot : ring) {
					const auto remote = DuplicateToGame(slot.localHandle);
					if (!remote) {
						closeUnannouncedHandles();
						ReleaseRing();
						return false;
					}
					remoteHandles.push_back(remote);
					slots.push_back(reinterpret_cast<std::uint64_t>(remote));
				}
				HANDLE produceLocal = nullptr;
				if (FAILED(produceFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
						&produceLocal))) {
					log.Error("produce fence CreateSharedHandle failed");
					closeUnannouncedHandles();
					ReleaseRing();
					return false;
				}
				const auto produceRemote = DuplicateToGame(produceLocal);
				::CloseHandle(produceLocal);
				if (!produceRemote) {
					closeUnannouncedHandles();
					ReleaseRing();
					return false;
				}
				remoteHandles.push_back(produceRemote);
				if (!Send(msg::ToJson(msg::Textures{
					.width = a_width,
					.height = a_height,
					.slots = std::move(slots),
					.produceFence = reinterpret_cast<std::uint64_t>(produceRemote),
					.adapterLuidLow = graphicsAdapterLuid.LowPart,
					.adapterLuidHigh = static_cast<std::uint32_t>(graphicsAdapterLuid.HighPart),
				}))) {
					closeUnannouncedHandles();
					ReleaseRing();
					return false;
				}
				log.InfoFwd(std::format("shared texture ring ready {}x{} ({} slots)", a_width, a_height, kRingSlots));
				return true;
			}

			// Host STA: publish one captured surface through the ring.
			void PublishFrame(ID3D11Texture2D* a_source, std::uint32_t a_width,
				std::uint32_t a_height, std::uint64_t a_presentationEpoch)
			{
				pendingCaptureEpoch = 0; // a newer capture supersedes any queued pixels
				if (TryPublishFrame(a_source, a_width, a_height, a_presentationEpoch)) return;
				if (captureClosing) return;
				// WGC's pool will recycle a_source. Keep one private copy so the final
				// frame of a short animation can be retried when a slot is acknowledged.
				D3D11_TEXTURE2D_DESC desc{};
				if (pendingCapture) pendingCapture->GetDesc(&desc);
				if (desc.Width != a_width || desc.Height != a_height) pendingCapture.Reset();
				if (!pendingCapture) {
					a_source->GetDesc(&desc);
					desc.MiscFlags = 0;
					if (FAILED(device->CreateTexture2D(&desc, nullptr, &pendingCapture))) return;
				}
				context->CopyResource(pendingCapture.Get(), a_source);
				pendingCaptureEpoch = a_presentationEpoch;
			}

			void RetryPendingCapture()
			{
				if (!pendingCapture || !pendingCaptureEpoch) return;
				if (pendingCaptureEpoch != presentationEpoch || !captureHasVisibleView) {
					pendingCaptureEpoch = 0;
					return;
				}
				D3D11_TEXTURE2D_DESC desc{};
				pendingCapture->GetDesc(&desc);
				if (desc.Width != width || desc.Height != height) {
					pendingCaptureEpoch = 0; // a resize superseded these pixels
					return;
				}
				if (TryPublishFrame(pendingCapture.Get(), desc.Width, desc.Height, pendingCaptureEpoch)) {
					pendingCaptureEpoch = 0;
				}
			}

			// Acknowledgement means the consumer has stopped using the slot and every GPU read has completed. Current stays reserved.
			bool TryPublishFrame(ID3D11Texture2D* a_source, std::uint32_t a_width, std::uint32_t a_height, std::uint64_t a_presentationEpoch)
			{
				if (captureClosing) return false;
				if (!EnsureRing(a_width, a_height)) return false;

				auto writableSlot = kRingSlots;
				for (std::uint32_t offset = 0; offset < kRingSlots; ++offset) {
					const auto candidate = (ringWrite + offset) % kRingSlots;
					if (ring[candidate].lastSerial == 0 ||
						ackedSerials[candidate] >= ring[candidate].lastSerial) {
						writableSlot = candidate;
						break;
					}
				}
				if (writableSlot == kRingSlots) {
					++consumeLagDrops;
					if (consumeLagDrops == 1 || consumeLagDrops % 300 == 0) {
						log.Info(std::format("capture backpressure (all {} slots reserved, {} deferrals); retaining latest capture", kRingSlots, consumeLagDrops));
					}
					return false;
				}
				auto& slot = ring[writableSlot];

				if (slot.texture.Get() != a_source) {
					context->CopyResource(slot.texture.Get(), a_source);
				}

				const auto serial = ++frameSerial;
				slot.lastSerial = serial;
				lastSlot = writableSlot;
				ringWrite = (writableSlot + 1) % kRingSlots;
				context4->Signal(produceFence.Get(), serial);
				context->Flush();
				Send(msg::ToJson(msg::Frame{ .slot = lastSlot, .serial = serial,
					.width = a_width, .height = a_height,
					.presentationEpoch = a_presentationEpoch }));
				if (serial == 1) {
					log.InfoFwd(std::format("first frame published ({}x{})", a_width, a_height));
				}
				return true;
			}

			// The game allocates epochs monotonically, but reveals and viewport changes complete out of order here.
			bool AdvancePresentation(std::uint64_t a_epoch)
			{
				if (a_epoch <= presentationEpoch) return false;
				presentationEpoch = a_epoch;
				return true;
			}

			bool PromotePresentation(View& a_view)
			{
				return AdvancePresentation(std::exchange(a_view.pendingPresentationEpoch, 0ull));
			}

			bool AdvanceChangedPresentation(std::uint64_t a_epoch)
			{
				if (a_epoch <= presentationEpoch) return false;
				// STA serialization prevents races, but queued pixels still belong to the old layout. Discard them before assigning the new epoch, even when capture dimensions are unchanged.
				if (framePool) {
					try {
						framePool.Recreate(captureDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, winrt::Windows::Graphics::SizeInt32{ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height) });
					} catch (const winrt::hresult_error& a_error) {
						log.Error(std::format("could not drain stale capture frames before presentation epoch {}: {}", ssssssssa_epoch, ToUtf8(a_error.message())));
						return false;
					}
				}
				return AdvancePresentation(a_epoch);
			}

			bool PromoteChangedPresentation(View& a_view)
			{
				const bool advanced = AdvanceChangedPresentation(a_view.pendingPresentationEpoch);
				// Clear completed or stale reveals; retain the request if pool recreation failed.
				if (a_view.pendingPresentationEpoch <= presentationEpoch) {
					a_view.pendingPresentationEpoch = 0;
				}
				return advanced;
			}

			void RepublishLatest()
			{
				if (!ring[0].texture || ring[lastSlot].lastSerial == 0) return;
				// Copying from a held slot is read-only. If the source itself is free, publication can reuse its pixels without a self-copy.
				// A full ring keeps these pixels as the pending capture, retried on the next acknowledgement.
				PublishFrame(ring[lastSlot].texture.Get(), ringWidth, ringHeight, presentationEpoch);
			}

			View* FindView(std::string_view a_id)
			{
				for (auto& view : views) {
					if (view->id == a_id) return view.get();
				}
				return nullptr;
			}

			View* ResolveView(const json& a_msg)
			{
				if (const auto it = a_msg.find("view"); it != a_msg.end() && it->is_string()) {
					return FindView(it->get<std::string>());
				}
				return nullptr;
			}

			View& CreateView(const std::string& a_id)
			{
				auto owned = std::make_unique<View>();
				owned->id = a_id;
				owned->generation = nextViewGeneration++;
				const auto slash = a_id.find('/');
				owned->modId = ToWide(a_id.substr(0, slash));
				owned->viewName = ToWide(a_id.substr(slash + 1));
				owned->window = ::CreateWindowExW(0, L"STATIC", L"OSFUI WebView2 View", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hostWindow, nullptr, ::GetModuleHandleW(nullptr), nullptr);
				if (!owned->window) {
					FailHost("view-window", HRESULT_FROM_WIN32(::GetLastError()), "child HWND creation failed", a_id);
				}
				views.push_back(std::move(owned));
				auto& view = *views.back();
				RefreshCaptureVisibility();
				if (!inputTarget) inputTarget = &view;
				RequestController(view);
				return view;
			}

