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
				ComPtr<ID3D10Multithread> multithread;
				if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
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
				// The root stays visible for the lifetime of the capture; per-view
				// visibility lives on each view's child visual.
				rootVisual.IsVisible(true);
				return true;
			}

			bool CreateWindows()
			{
				// A visible 1x1 child beneath a visible (offscreen) top-level owned
				// by this STA; the child is reparented beneath the game window once
				// Chromium is up. The bootstrap must never activate: creating a
				// visible top-level popup in the freshly launched browser host can otherwise
				// make Windows foreground the browser host and background Starfield.
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
				s_hostInputApp = this;
				::SetLastError(ERROR_SUCCESS);
				hostWindowProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
					hostWindow, GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(&HostInputWndProc)));
				if (!hostWindowProc && ::GetLastError() != ERROR_SUCCESS) {
					log.Error(std::format("browser-host child HWND subclass failed ({})", ::GetLastError()));
					s_hostInputApp = nullptr;
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

			bool EnsureFences()
			{
				if (produceFence && consumeFence) return true;
				auto hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
					IID_PPV_ARGS(&produceFence));
				if (FAILED(hr)) {
					log.Error(std::format("CreateFence(produce) failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
				}
				hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
					IID_PPV_ARGS(&consumeFence));
				if (FAILED(hr)) {
					log.Error(std::format("CreateFence(consume) failed (0x{:08X})",
						static_cast<unsigned>(hr)));
					return false;
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

			// Capture thread. Returns false when the ring could not be built.
			bool EnsureRing(std::uint32_t a_width, std::uint32_t a_height)
			{
				if (ring[0].texture && ringWidth == a_width && ringHeight == a_height) {
					return true;
				}
				ReleaseRing();
				if (!EnsureFences()) return false;

				D3D11_TEXTURE2D_DESC desc{};
				desc.Width = a_width;
				desc.Height = a_height;
				desc.MipLevels = 1;
				desc.ArraySize = 1;
				desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				desc.SampleDesc.Count = 1;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				// Preferred: NT-handle shared without a keyed mutex — the D3D12 side
				// has no IDXGIKeyedMutex; the shared fences do the synchronizing.
				desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
				ringKeyedMutex = false;
				auto hr = device->CreateTexture2D(&desc, nullptr, &ring[0].texture);
				if (FAILED(hr)) {
					// Some drivers only accept NTHANDLE together with KEYED_MUTEX.
					desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
						D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
					ringKeyedMutex = true;
					hr = device->CreateTexture2D(&desc, nullptr, &ring[0].texture);
					if (FAILED(hr)) {
						log.Error(std::format(
							"shared texture creation failed both modes (0x{:08X})",
							static_cast<unsigned>(hr)));
						return false;
					}
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
					if (FAILED(slot.texture.As(&resource)) ||
						FAILED(resource->CreateSharedHandle(nullptr,
							DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
							nullptr, &slot.localHandle))) {
						log.Error("CreateSharedHandle failed");
						ReleaseRing();
						return false;
					}
				}
				ringWidth = a_width;
				ringHeight = a_height;
				ringWrite = 0;

				// Duplicate everything into the game and announce the new ring.
				json slots = json::array();
				for (auto& slot : ring) {
					const auto remote = DuplicateToGame(slot.localHandle);
					if (!remote) {
						ReleaseRing();
						return false;
					}
					slots.push_back(reinterpret_cast<std::uint64_t>(remote));
				}
				HANDLE produceLocal = nullptr;
				HANDLE consumeLocal = nullptr;
				if (FAILED(produceFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
						&produceLocal)) ||
					FAILED(consumeFence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
						&consumeLocal))) {
					log.Error("fence CreateSharedHandle failed");
					ReleaseRing();
					return false;
				}
				const auto produceRemote = DuplicateToGame(produceLocal);
				const auto consumeRemote = DuplicateToGame(consumeLocal);
				::CloseHandle(produceLocal);
				::CloseHandle(consumeLocal);
				if (!produceRemote || !consumeRemote) {
					ReleaseRing();
					return false;
				}
				Send(json{
					{ "type", "textures" },
					{ "width", a_width },
					{ "height", a_height },
					{ "slots", std::move(slots) },
					{ "produceFence", reinterpret_cast<std::uint64_t>(produceRemote) },
					{ "consumeFence", reinterpret_cast<std::uint64_t>(consumeRemote) },
					{ "keyedMutex", ringKeyedMutex },
					{ "adapterLuidLow", graphicsAdapterLuid.LowPart },
					{ "adapterLuidHigh", static_cast<std::uint32_t>(graphicsAdapterLuid.HighPart) },
				});
				log.InfoFwd(std::format(
					"shared texture ring ready {}x{} ({} slots, keyedMutex={})",
					a_width, a_height, kRingSlots, ringKeyedMutex));
				return true;
			}

			// Capture thread: publish one captured surface through the ring.
			void PublishFrame(ID3D11Texture2D* a_source, std::uint32_t a_width,
				std::uint32_t a_height, std::uint64_t a_presentationEpoch)
			{
				std::scoped_lock lock(ringMutex);
				if (captureClosing.load()) return;
				if (!EnsureRing(a_width, a_height)) return;

				auto& slot = ring[ringWrite];
				// Slot reuse guard: the game GPU-signals `consume` with each serial it
				// finished reading and pipe-acks frames it skipped, so this wait is
				// normally already satisfied. Bounded, so a wedged consumer costs
				// 50 ms rather than a deadlock.
				const auto consumed = [this] {
					return (std::max)(consumeFence->GetCompletedValue(), ackedSerial.load());
				};
				if (slot.lastSerial != 0 && consumed() < slot.lastSerial) {
					const HANDLE evt = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
					if (!evt) {
						log.Warn("could not create the consume wait event; dropping captured frame");
						return;
					}
					const auto waitHr = consumeFence->SetEventOnCompletion(slot.lastSerial, evt);
					if (FAILED(waitHr)) {
						::CloseHandle(evt);
						log.Warn(std::format(
							"consume wait registration failed (hr=0x{:08X}); dropping captured frame",
							static_cast<std::uint32_t>(waitHr)));
						return;
					}
					const auto deadline = ::GetTickCount64() + 50;
					while (consumed() < slot.lastSerial && ::GetTickCount64() < deadline) {
						::WaitForSingleObject(evt, 10);
					}
					const bool stillBusy = consumed() < slot.lastSerial;
					::CloseHandle(evt);
					if (stillBusy) {
						++consumeWaitTimeouts;
						if (consumeWaitTimeouts == 1 || consumeWaitTimeouts % 300 == 0) {
							log.Warn(std::format(
								"consume lagging (slot serial {}, completed {}, {} drops); captured frame dropped",
								slot.lastSerial, consumed(), consumeWaitTimeouts));
						}
						return;
					}
				}

				if (ringKeyedMutex) {
					ComPtr<IDXGIKeyedMutex> mutex;
					if (SUCCEEDED(slot.texture.As(&mutex))) {
						if (mutex->AcquireSync(0, 50) != S_OK) {
							return;  // contended/abandoned; drop this frame
						}
						context->CopyResource(slot.texture.Get(), a_source);
						mutex->ReleaseSync(0);
					} else {
						return;  // keyed-mutex QI unexpectedly failed; drop rather than publish an uncopied slot
					}
				} else {
					context->CopyResource(slot.texture.Get(), a_source);
				}

				const auto serial = ++frameSerial;
				slot.lastSerial = serial;
				lastSlot = ringWrite;
				ringWrite = (ringWrite + 1) % kRingSlots;
				context4->Signal(produceFence.Get(), serial);
				// Flush so the copy + signal reach the GPU now: the consumer's wait
				// must not depend on this context's next natural flush.
				context->Flush();
				Send(json{
					{ "type", "frame" }, { "slot", lastSlot }, { "serial", serial },
					{ "width", a_width }, { "height", a_height },
					{ "presentationEpoch", a_presentationEpoch } });
				if (serial == 1) {
					log.InfoFwd(std::format("first frame published ({}x{})", a_width, a_height));
				}
			}

			// Publish the requested epoch only after the STA has made its visual
			// visible. The next real WGC capture then proves that the frame belongs
			// to this open rather than to the transparent closed presentation.
			// Returns whether a new epoch was actually promoted (a redundant show
			// of an already-current epoch is not a promotion).
			bool PromotePresentation(View& a_view)
			{
				const auto requested = std::exchange(a_view.pendingPresentationEpoch, 0ull);
				if (requested == 0 ||
					requested == presentationEpoch.load(std::memory_order_relaxed)) {
					return false;
				}
				presentationEpoch.store(requested, std::memory_order_release);
				return true;
			}

			// A composition-changing reveal must discard every capture queued
			// before visibility changed. The mutex also makes the frame callback
			// acquire a frame and its epoch as one snapshot.
			bool PromoteChangedPresentation(View& a_view)
			{
				std::scoped_lock epochLock(captureEpochMutex);
				if (framePool) {
					try {
						framePool.Recreate(captureDevice,
							winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
							3, winrt::Windows::Graphics::SizeInt32{
								static_cast<std::int32_t>(width),
								static_cast<std::int32_t>(height) });
					} catch (const winrt::hresult_error& a_error) {
						log.Error(std::format(
							"view '{}': could not drain stale capture frames before reveal: {}",
							a_view.id, ToUtf8(a_error.message())));
						return false;
					}
				}
				return PromotePresentation(a_view);
			}

			// STA thread, for shows where the composition does not change (the
			// visual never left the screen): WGC only captures on damage, so a
			// static page would never emit a frame carrying the just-promoted
			// epoch and the game's reveal gate would starve into its timeout.
			// Re-send the newest ring pixels under a fresh serial stamped with the
			// current epoch. Only safe on those no-change paths — after a real
			// closed->open the last capture is the transparent closed state and
			// must never be re-stamped (the bug epochs exist to prevent).
			void RepublishLatest()
			{
				std::scoped_lock lock(ringMutex);
				// The last slot must actually hold pixels: after a resize recreated
				// the ring, nothing is republishable until the first capture lands
				// in the new ring.
				if (!ring[0].texture || ring[lastSlot].lastSerial == 0) {
					return;
				}
				const auto serial = ++frameSerial;
				ring[lastSlot].lastSerial = serial;
				context4->Signal(produceFence.Get(), serial);
				context->Flush();
				Send(json{
					{ "type", "frame" }, { "slot", lastSlot }, { "serial", serial },
					{ "width", ringWidth }, { "height", ringHeight },
					{ "presentationEpoch",
						presentationEpoch.load(std::memory_order_relaxed) } });
			}

			View* FindView(std::string_view a_id)
			{
				for (auto& view : views) {
					if (view->id == a_id) return view.get();
				}
				return nullptr;
			}

			// View-scoped messages carry `view`; absent or unknown falls back to the
			// input-target view for compatibility with the single-view POC client.
			View* ResolveView(const json& a_msg)
			{
				if (const auto it = a_msg.find("view"); it != a_msg.end() && it->is_string()) {
					if (auto* view = FindView(it->get<std::string>())) return view;
				}
				return inputTarget;
			}

			View& CreateView(const std::string& a_id)
			{
				auto owned = std::make_unique<View>();
				owned->id = a_id;
				owned->hidden = defaultHidden;
				owned->window = ::CreateWindowExW(0, L"STATIC", L"OSFUI WebView2 View",
					WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hostWindow, nullptr,
					::GetModuleHandleW(nullptr), nullptr);
				if (!owned->window) {
					log.Error(std::format("view '{}': child HWND creation failed ({})",
						a_id, ::GetLastError()));
				}
				views.push_back(std::move(owned));
				auto& view = *views.back();
				if (!inputTarget) inputTarget = &view;
				RequestController(view);
				return view;
			}

			// View order maps to child order under the captured root: lower `order`
			// composites beneath, ties keep creation order. Rebuilt wholesale;
			// reorders are rare and the child count tiny.
