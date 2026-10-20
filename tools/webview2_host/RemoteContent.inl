			// Required APIs fail closed before any remote navigation. This is
			// per controller: trusted chrome in the same process keeps its bridge.
			HRESULT InstallRemoteContentPolicy(View& view)
			{
				ComPtr<ICoreWebView2Settings> base;
				auto hr = view.webView->get_Settings(&base);
				if (FAILED(hr) || !base) return FAILED(hr) ? hr : E_POINTER;
				ComPtr<ICoreWebView2Settings4> settings;
				if (FAILED(hr = base.As(&settings))) return hr;
				if (FAILED(hr = settings->put_IsWebMessageEnabled(FALSE))) return hr;
				if (FAILED(hr = settings->put_AreHostObjectsAllowed(FALSE))) return hr;
				if (FAILED(hr = settings->put_AreDefaultContextMenusEnabled(FALSE))) return hr;
				if (FAILED(hr = settings->put_AreDevToolsEnabled(devMode ? TRUE : FALSE))) return hr;
				if (FAILED(hr = settings->put_AreDefaultScriptDialogsEnabled(FALSE))) return hr;
				if (FAILED(hr = settings->put_IsStatusBarEnabled(FALSE))) return hr;
				if (FAILED(hr = settings->put_AreBrowserAcceleratorKeysEnabled(FALSE))) return hr;
				if (FAILED(hr = settings->put_IsPasswordAutosaveEnabled(FALSE))) return hr;
				if (FAILED(hr = settings->put_IsGeneralAutofillEnabled(FALSE))) return hr;

				ComPtr<ICoreWebView2_18> web;
				if (FAILED(hr = view.webView.As(&web))) return hr;
				EventRegistrationToken token{};
				hr = web->add_PermissionRequested(
					Callback<ICoreWebView2PermissionRequestedEventHandler>(
						[](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
							return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = web->add_DownloadStarting(
					Callback<ICoreWebView2DownloadStartingEventHandler>(
						[](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
							args->put_Handled(TRUE);
							return args->put_Cancel(TRUE);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = web->add_LaunchingExternalUriScheme(
					Callback<ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
						[](ICoreWebView2*, ICoreWebView2LaunchingExternalUriSchemeEventArgs* args) -> HRESULT {
							return args->put_Cancel(TRUE);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = web->add_BasicAuthenticationRequested(
					Callback<ICoreWebView2BasicAuthenticationRequestedEventHandler>(
						[](ICoreWebView2*, ICoreWebView2BasicAuthenticationRequestedEventArgs* args) -> HRESULT {
							return args->put_Cancel(TRUE);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = web->add_ClientCertificateRequested(
					Callback<ICoreWebView2ClientCertificateRequestedEventHandler>(
						[](ICoreWebView2*, ICoreWebView2ClientCertificateRequestedEventArgs* args) -> HRESULT {
							args->put_Handled(TRUE);
							return args->put_Cancel(TRUE);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = web->add_ServerCertificateErrorDetected(
					Callback<ICoreWebView2ServerCertificateErrorDetectedEventHandler>(
						[](ICoreWebView2*, ICoreWebView2ServerCertificateErrorDetectedEventArgs* args) -> HRESULT {
							return args->put_Action(COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_CANCEL);
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				hr = web->add_ScriptDialogOpening(
					Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
						[](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* args) -> HRESULT {
							COREWEBVIEW2_SCRIPT_DIALOG_KIND kind{};
							if (SUCCEEDED(args->get_Kind(&kind)) && kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_BEFOREUNLOAD)
								return args->Accept(); // A page cannot trap host-initiated navigation/close.
							return S_OK; // No Accept or deferral: dismiss alert, cancel prompt/confirm.
						}).Get(), &token);
				if (FAILED(hr)) return hr;
				return web->add_NewWindowRequested(
					Callback<ICoreWebView2NewWindowRequestedEventHandler>(
						[this, target = &view](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
							const auto handled = args->put_Handled(TRUE);
							if (FAILED(handled)) {
								ReportSecurityFailure(*target, handled, "could not suppress popup");
								return S_OK;
							}
							LPWSTR raw{};
							if (SUCCEEDED(args->get_Uri(&raw)) && raw) {
								// Popups only reuse this content controller, never create a native window.
								if (IsAllowedRemoteNavigationUri(raw)) target->webView->Navigate(raw);
							}
							::CoTaskMemFree(raw);
							return S_OK;
						}).Get(), &token);
			}
