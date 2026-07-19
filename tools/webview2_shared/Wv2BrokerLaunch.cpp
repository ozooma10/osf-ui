#include "Wv2BrokerLaunch.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

#include <comdef.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <format>

// shellapi.h (pulled in by shlobj.h) macro-renames ShellExecute ->
// ShellExecuteW, which would mangle the IShellDispatch2::ShellExecute METHOD
// name below. The COM method has no A/W variants — undo the macro.
#ifdef ShellExecute
#	undef ShellExecute
#endif

#pragma comment(lib, "taskschd.lib")

using Microsoft::WRL::ComPtr;

namespace osfui::wv2
{
	namespace
	{
		struct ComApartment
		{
			// The caller may already be in an STA (renderer worker) or have no
			// apartment at all (tool thread); accept both.
			HRESULT hr;
			ComApartment() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
			~ComApartment()
			{
				if (SUCCEEDED(hr)) {
					::CoUninitialize();
				}
			}
			[[nodiscard]] bool Usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
		};

		std::string Hr(HRESULT a_hr)
		{
			return std::format("0x{:08X}", static_cast<unsigned>(a_hr));
		}

		// The classic "run as the shell user / out of our process tree" hop:
		// desktop shell view -> automation object -> IShellDispatch2 lives in
		// explorer.exe, so its ShellExecute makes explorer the parent.
		HRESULT ExplorerShellExecute(const std::wstring& a_exe, const std::wstring& a_args,
			std::string& a_detail)
		{
			ComPtr<IShellWindows> shellWindows;
			auto hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
				IID_PPV_ARGS(&shellWindows));
			if (FAILED(hr)) {
				a_detail += "CoCreateInstance(ShellWindows)=" + Hr(hr) + "; ";
				return hr;
			}

			VARIANT loc;
			loc.vt = VT_I4;
			loc.lVal = CSIDL_DESKTOP;
			VARIANT empty{};
			long hwnd = 0;
			ComPtr<IDispatch> dispatch;
			hr = shellWindows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd,
				SWFO_NEEDDISPATCH, &dispatch);
			if (hr != S_OK || !dispatch) {
				a_detail += "FindWindowSW(desktop)=" + Hr(hr) + "; ";
				return FAILED(hr) ? hr : E_FAIL;
			}

			ComPtr<IServiceProvider> services;
			hr = dispatch.As(&services);
			if (FAILED(hr)) {
				a_detail += "QI IServiceProvider=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IShellBrowser> browser;
			hr = services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
			if (FAILED(hr)) {
				a_detail += "QueryService(STopLevelBrowser)=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IShellView> view;
			hr = browser->QueryActiveShellView(&view);
			if (FAILED(hr)) {
				a_detail += "QueryActiveShellView=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IDispatch> viewDispatch;
			hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&viewDispatch));
			if (FAILED(hr)) {
				a_detail += "GetItemObject(background)=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IShellFolderViewDual> folderView;
			hr = viewDispatch.As(&folderView);
			if (FAILED(hr)) {
				a_detail += "QI IShellFolderViewDual=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IDispatch> application;
			hr = folderView->get_Application(&application);
			if (FAILED(hr) || !application) {
				a_detail += "get_Application=" + Hr(hr) + "; ";
				return FAILED(hr) ? hr : E_FAIL;
			}
			ComPtr<IShellDispatch2> shell;
			hr = application.As(&shell);
			if (FAILED(hr)) {
				a_detail += "QI IShellDispatch2=" + Hr(hr) + "; ";
				return hr;
			}

			const _bstr_t file(a_exe.c_str());
			_variant_t args(a_args.c_str());
			_variant_t dir;   // default
			_variant_t verb;  // default ("open")
			_variant_t show(static_cast<long>(SW_HIDE));
			hr = shell->ShellExecute(file, args, dir, verb, show);
			if (FAILED(hr)) {
				a_detail += "IShellDispatch2::ShellExecute=" + Hr(hr) + "; ";
			}
			return hr;
		}

		// One-shot scheduled task, run immediately in the caller's interactive
		// session, then deleted. The spawned process is a child of svchost's
		// task host — also outside the game's tree.
		HRESULT TaskSchedulerExecute(const std::wstring& a_exe, const std::wstring& a_args,
			std::string& a_detail)
		{
			ComPtr<ITaskService> service;
			auto hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&service));
			if (FAILED(hr)) {
				a_detail += "CoCreateInstance(TaskScheduler)=" + Hr(hr) + "; ";
				return hr;
			}
			hr = service->Connect(_variant_t{}, _variant_t{}, _variant_t{}, _variant_t{});
			if (FAILED(hr)) {
				a_detail += "ITaskService::Connect=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<ITaskFolder> root;
			hr = service->GetFolder(_bstr_t(L"\\"), &root);
			if (FAILED(hr)) {
				a_detail += "GetFolder=" + Hr(hr) + "; ";
				return hr;
			}

			ComPtr<ITaskDefinition> task;
			hr = service->NewTask(0, &task);
			if (FAILED(hr)) {
				a_detail += "NewTask=" + Hr(hr) + "; ";
				return hr;
			}
			{
				ComPtr<ITaskSettings> settings;
				if (SUCCEEDED(task->get_Settings(&settings))) {
					settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
					settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
					settings->put_StartWhenAvailable(VARIANT_TRUE);
					// No execution time cap: this "task" is a long-lived host
					// process ("PT0S" disables the 72h default limit).
					settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
				}
				ComPtr<IPrincipal> principal;
				if (SUCCEEDED(task->get_Principal(&principal))) {
					principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
					principal->put_RunLevel(TASK_RUNLEVEL_LUA);
				}
			}
			ComPtr<IActionCollection> actions;
			hr = task->get_Actions(&actions);
			if (FAILED(hr)) {
				a_detail += "get_Actions=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IAction> action;
			hr = actions->Create(TASK_ACTION_EXEC, &action);
			if (FAILED(hr)) {
				a_detail += "Actions::Create=" + Hr(hr) + "; ";
				return hr;
			}
			ComPtr<IExecAction> exec;
			hr = action.As(&exec);
			if (FAILED(hr)) {
				a_detail += "QI IExecAction=" + Hr(hr) + "; ";
				return hr;
			}
			exec->put_Path(_bstr_t(a_exe.c_str()));
			exec->put_Arguments(_bstr_t(a_args.c_str()));

			const auto taskName = std::format(L"OSFUI-WebView2-Host-{}", ::GetCurrentProcessId());
			// Clear a leftover from a crashed earlier run before registering.
			root->DeleteTask(_bstr_t(taskName.c_str()), 0);

			ComPtr<IRegisteredTask> registered;
			hr = root->RegisterTaskDefinition(_bstr_t(taskName.c_str()), task.Get(),
				TASK_CREATE_OR_UPDATE, _variant_t{}, _variant_t{},
				TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), &registered);
			if (FAILED(hr) || !registered) {
				a_detail += "RegisterTaskDefinition=" + Hr(hr) + "; ";
				return FAILED(hr) ? hr : E_FAIL;
			}

			ComPtr<IRunningTask> running;
			hr = registered->Run(_variant_t{}, &running);
			// Delete right away in every case: the already-running process
			// survives its task's deletion.
			root->DeleteTask(_bstr_t(taskName.c_str()), 0);
			if (FAILED(hr)) {
				a_detail += "IRegisteredTask::Run=" + Hr(hr) + "; ";
				return hr;
			}
			return S_OK;
		}

		bool DirectCreateProcess(const std::wstring& a_exe, const std::wstring& a_args,
			std::string& a_detail)
		{
			std::wstring commandLine = L"\"" + a_exe + L"\" " + a_args;
			STARTUPINFOW si{ sizeof(si) };
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION pi{};
			if (!::CreateProcessW(a_exe.c_str(), commandLine.data(), nullptr, nullptr,
					FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
				a_detail += "CreateProcess=" + std::to_string(::GetLastError()) + "; ";
				return false;
			}
			::CloseHandle(pi.hThread);
			::CloseHandle(pi.hProcess);
			return true;
		}
	}

	const char* LaunchMethodName(LaunchMethod a_method)
	{
		switch (a_method) {
		case LaunchMethod::kExplorer: return "explorer";
		case LaunchMethod::kTaskScheduler: return "task-scheduler";
		case LaunchMethod::kDirect: return "direct";
		default: return "none";
		}
	}

	LaunchResult LaunchDetached(const std::wstring& a_exe, const std::wstring& a_args,
		bool a_preferBroker)
	{
		LaunchResult result;
		if (a_preferBroker) {
			ComApartment com;
			if (com.Usable()) {
				if (SUCCEEDED(ExplorerShellExecute(a_exe, a_args, result.detail))) {
					result.ok = true;
					result.method = LaunchMethod::kExplorer;
					return result;
				}
				if (SUCCEEDED(TaskSchedulerExecute(a_exe, a_args, result.detail))) {
					result.ok = true;
					result.method = LaunchMethod::kTaskScheduler;
					return result;
				}
			} else {
				result.detail += "CoInitializeEx=" + Hr(com.hr) + "; ";
			}
		}
		if (DirectCreateProcess(a_exe, a_args, result.detail)) {
			result.ok = true;
			result.method = LaunchMethod::kDirect;
		}
		return result;
	}
}
