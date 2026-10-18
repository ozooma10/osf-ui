#include "Render/BrowserHostRuntime.h"
#include "Views/ViewCache.h"
#include "Core/Utf8Path.h"
#include <format>
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

namespace OSFUI
{
    namespace
    {
        bool HasMarkOfTheWeb(const std::filesystem::path& file)
        {
            return ::GetFileAttributesW((file.native() + L":Zone.Identifier").c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // Only MO2's USVFS hides the shipped files from the helper; real installs launch in place.
        bool UsesVirtualFilesystem() { return ::GetModuleHandleW(L"usvfs_x64.dll") != nullptr; }

        void PruneCopies(const std::filesystem::path& root, const std::filesystem::path& keep, std::string_view label)
        {
            const auto result = ViewCache::Scavenge(root, keep);
            if (result.removed) REX::INFO("Removed {} old {} cop{}", result.removed, label, result.removed == 1 ? "y" : "ies");
            if (result.failed) REX::WARN("{} old {} cop{} still in use; retrying next launch", result.failed, label, result.failed == 1 ? "y" : "ies");
        }
    }

    BrowserHostLayout HostLayout(osfui::browser::Backend backend, const std::filesystem::path& dataDir)
    {
        if (backend == osfui::browser::Backend::Cef)
            return { dataDir / "bin" / "cef" / "osfui_cef_host.exe", "CEF", "OSF UI.cef-host.log" };
        return { dataDir / "bin" / "osfui_webview2_host.exe", "WebView2", "OSF UI.webview2-host.log" };
    }

    std::optional<std::filesystem::path> PrepareBrowserBroker(const std::filesystem::path& dataDir,
        const std::filesystem::path& localRoot)
    {
        if (!UsesVirtualFilesystem()) return dataDir / "bin" / "broker" / "osfui_browser_broker.exe";
        std::string error;
        const auto prepared = ViewCache::Prepare(dataDir / "bin" / "broker", localRoot / "browser-broker",
            "osfui-browser-broker-1", std::format("{}-{}", ::GetCurrentProcessId(), ::GetTickCount64()), error);
        if (!prepared) {
            REX::ERROR("Browser broker preparation failed: {}", error);
            return std::nullopt;
        }
        return prepared->generation / "osfui_browser_broker.exe";
    }

    bool ValidateHostPayload(osfui::browser::Backend backend, const std::filesystem::path& executable, std::string& error)
    {
        std::error_code ec;
        const auto require = [&](const std::filesystem::path& path) {
            if (std::filesystem::is_regular_file(path, ec)) return true;
            error = "Browser runtime file is missing: " + Utf8Path(path) + ". Reinstall the complete selected OSF UI edition.";
            return false;
        };
        if (!require(executable)) return false;
        const auto bin = backend == osfui::browser::Backend::Cef ? executable.parent_path().parent_path() : executable.parent_path();
        if (!require(bin / "broker" / "osfui_browser_broker.exe")) return false;
        if (backend == osfui::browser::Backend::Cef) {
            for (const auto* name : { "osfui_cef_host.dll", "libcef.dll", "icudtl.dat", "resources.pak", "cef.lock.json" })
                if (!require(executable.parent_path() / name)) return false;
        }
        return true;
    }

    std::optional<std::filesystem::path> PrepareHostExecutable(osfui::browser::Backend backend,
        const std::filesystem::path& source, const std::filesystem::path& localRoot)
	{
		std::filesystem::path mirror;
		if (backend == osfui::browser::Backend::Cef) {
			if (!UsesVirtualFilesystem()) return source;
			// MO2's virtual DLL/resource tree must be materialized alongside the bootstrap.
			// Separate generations avoid replacing a runtime still held by Chromium children.
			std::string error;
			const auto prepared = ViewCache::Prepare(source.parent_path(),
				localRoot / "cef-runtime", "osfui-cef-runtime-1",
				std::format("{}-{}", ::GetCurrentProcessId(), ::GetTickCount64()), error);
			if (!prepared) {
				REX::ERROR("CEF runtime preparation failed: {}", error);
				return std::nullopt;
			}
			mirror = prepared->generation / source.filename();
			REX::INFO("CEF runtime {} at {}", prepared->reused ? "reused" : "prepared", Utf8Path(prepared->generation));
			return mirror;
		}
		const auto mirrorDir = localRoot / "bin";
		mirror = mirrorDir / "osfui_webview2_host.exe";
		std::error_code ec;
		std::filesystem::create_directories(mirrorDir, ec);
		ec.clear();
		const auto sourceSize = std::filesystem::file_size(source, ec);
		if (ec) {
			REX::ERROR("BrowserHostClient: browser-host executable missing at {} ({})",
				Utf8Path(source), ec.message());
			return std::nullopt;
		}
		ec.clear();
		const auto mirrorSize = std::filesystem::file_size(mirror, ec);
		const bool haveMirror = !ec;
		const bool sameSize = haveMirror && mirrorSize == sourceSize;
		const bool sameTime = haveMirror &&
			std::filesystem::last_write_time(source, ec) <=
				std::filesystem::last_write_time(mirror, ec);
		if (!(sameSize && sameTime)) {
			ec.clear();
			std::filesystem::copy_file(source, mirror,
				std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) {
				if (sameSize) {
					// Reuse an in-use mirror only when its content matches the shipped host.
					REX::WARN("BrowserHostClient: browser-host executable mirror busy; reusing existing copy ({})", ec.message());
				} else {
					REX::ERROR("BrowserHostClient: browser-host executable mirror copy failed ({})", ec.message());
					return std::nullopt;
				}
			}
		}
		// Strip the copied Zone.Identifier so a hidden SmartScreen prompt cannot block launch.
		if (HasMarkOfTheWeb(mirror)) {
			if (::DeleteFileW((mirror.native() + L":Zone.Identifier").c_str())) {
				REX::INFO("BrowserHostClient: stripped Mark-of-the-Web from the "
						  "browser-host executable mirror");
			} else {
				REX::WARN("BrowserHostClient: browser-host executable mirror carries "
						  "Mark-of-the-Web and stripping it failed ({}) — SmartScreen "
						  "may silently block browser-host launch", ::GetLastError());
			}
		}
		return mirror;
	}

    void PruneBrowserRuntimeCopies(const std::filesystem::path& localRoot,
        const std::filesystem::path& hostExe, const std::filesystem::path& brokerExe)
    {
        // Keeps only the generations this launch uses; switching editions or leaving MO2 removes them all.
        PruneCopies(localRoot / "cef-runtime", hostExe.parent_path(), "CEF runtime");
        PruneCopies(localRoot / "browser-broker", brokerExe.parent_path(), "browser broker");
    }

}
