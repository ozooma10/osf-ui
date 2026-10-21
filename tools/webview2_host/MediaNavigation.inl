			void CompleteNavigation(View& view, bool succeeded, int error, std::string source)
			{
				view.mediaNavigation.End();
				view.documentNavigation.Complete(succeeded);
				view.browserLoading = false;
				view.browserLoadSucceeded = succeeded;
				if (view.audio) view.audio->SetLoading(!succeeded);
				PublishBrowserState(view);
				Send(msg::ToJson(msg::LoadEvent{
					.view = view.id, .failed = !succeeded, .url = std::move(source),
					.description = succeeded ? "" : "The page could not finish loading. Try reloading.",
					.code = succeeded ? 0 : error }));
			}

			void CancelNavigation(View& view, std::uint64_t navigationId)
			{
				LPWSTR raw{};
				std::wstring source;
				if (view.webView && SUCCEEDED(view.webView->get_Source(&raw)) && raw) source = raw;
				::CoTaskMemFree(raw);
				const bool allowed = IsAllowedContentDocument(view.remoteContent, source, view.modId, view.viewName);
				if (!view.documentNavigation.Cancel(navigationId, allowed)) return;
				view.mediaNavigation.End(); // Invalidate an outstanding media metadata query too.
				view.browserLoading = false;
				view.browserLoadSucceeded = view.documentNavigation.usable;
				if (view.audio) view.audio->SetLoading(!view.documentNavigation.usable);
				if (view.documentNavigation.domReady && !view.domSeen) {
					view.domSeen = true;
					InitializeCdpInput(view);
					DrainQueuedViewWork(view);
				}
				PublishBrowserState(view);
				// Cancellation is not a failed host. Only acknowledge a document when
				// a commit or the previous completed navigation proves one is usable.
				if (view.documentNavigation.usable)
					Send(msg::ToJson(msg::LoadEvent{ .view = view.id, .url = ToUtf8(source) }));
			}

			void FinishMediaNavigation(View& view, bool accepted, std::string_view reason)
			{
				log.Info(std::format("view '{}': direct-media navigation {} {} ({})",
					view.id, view.mediaNavigation.navigationId, accepted ? "accepted" : "rejected", reason));
				CompleteNavigation(view, accepted, MediaNavigation::kConnectionAborted, view.mediaNavigation.source);
			}

			void TickMediaNavigation(View& view, std::uint64_t now)
			{
				if (!view.mediaNavigation.pending) return;
				if (view.mediaNavigation.Expired(now)) {
					FinishMediaNavigation(view, false, "document metadata confirmation timed out");
					return;
				}
				if (!view.webView || !view.domSeen || view.mediaQueryInFlight || !view.mediaNavigation.Ready(now)) return;
				view.mediaNavigation.queryStarted = true;
				view.mediaQueryInFlight = true;
				const auto id = view.id;
				const auto generation = view.generation;
				const auto navigationId = view.mediaNavigation.navigationId;
				const auto hr = view.webView->CallDevToolsProtocolMethod(L"Page.getFrameTree", L"{}",
					Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
						[this, id, generation, navigationId](HRESULT result, LPCWSTR payload) -> HRESULT {
							auto* current = FindView(id);
							if (!current || current->generation != generation) return S_OK;
							current->mediaQueryInFlight = false;
							if (quit.load() || rendererFatal || !current->domSeen ||
								!current->mediaNavigation.Matches(navigationId, ::GetTickCount64())) return S_OK;
							LPWSTR raw{};
							const bool sameSource = current->webView &&
								SUCCEEDED(current->webView->get_Source(&raw)) && raw &&
								ToUtf8(raw) == current->mediaNavigation.source;
							::CoTaskMemFree(raw);
							std::string mime;
							if (sameSource && SUCCEEDED(result) && payload) {
								constexpr std::size_t kMaxMetadataChars = 64 * 1024;
								std::size_t length{};
								while (length <= kMaxMetadataChars && payload[length]) ++length;
								if (length <= kMaxMetadataChars) {
									const auto parsed = Json::Parse(ToUtf8(std::wstring_view(payload, length)));
									if (parsed) mime = VerifiedMediaDocumentMime(*parsed, current->mediaNavigation.source);
								}
							}
							FinishMediaNavigation(*current, !mime.empty(), mime.empty() ? "main frame was not a verified media document" : mime);
							return S_OK;
						}).Get());
				if (FAILED(hr)) {
					view.mediaQueryInFlight = false;
					FinishMediaNavigation(view, false, "document metadata request failed");
				}
			}
