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

        // USVFS redirects the open but not handle path queries, so the kernel reports the backing file.
        std::optional<std::filesystem::path> BackingFile(const std::filesystem::path& file)
        {
            const HANDLE handle = ::CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
            std::wstring buffer(MAX_PATH, L'\0');
            DWORD length = 0;
            for (int attempt = 0; attempt < 2; ++attempt) {
                length = ::GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                if (length < buffer.size()) break;
                buffer.resize(length);
            }
            ::CloseHandle(handle);
            if (!length || length >= buffer.size()) return std::nullopt;
            buffer.resize(length);
            if (buffer.starts_with(LR"(\\?\UNC\)")) return LR"(\\)" + buffer.substr(8);
            if (buffer.starts_with(LR"(\\?\)")) return buffer.substr(4);
            return buffer;
        }

        bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
        {
            return ::_wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
        }

        // The one real folder backing every file of a virtual tree. MO2 can merge files from several
        // mods (or overwrite) into one virtual folder; any split, or a tree that is not redirected, is rejected.
        std::optional<std::filesystem::path> BackingFolder(const std::filesystem::path& root, std::string& why)
        {
            std::filesystem::path backing;
            std::size_t files = 0;
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const auto relative = it->path().lexically_relative(root);
                const auto real = BackingFile(it->path());
                if (!real) {
                    why = std::format("cannot resolve {} ({})", Utf8Path(relative), ::GetLastError());
                    return std::nullopt;
                }
                auto candidate = *real;
                for (auto part = relative.begin(); part != relative.end(); ++part) candidate = candidate.parent_path();
                if (!SamePath(candidate / relative, *real)) {
                    why = std::format("{} resolves to a different name ({})", Utf8Path(relative), Utf8Path(*real));
                    return std::nullopt;
                }
                if (backing.empty()) backing = candidate;
                else if (!SamePath(backing, candidate)) {
                    why = std::format("{} comes from {}, not {}", Utf8Path(relative), Utf8Path(candidate), Utf8Path(backing));
                    return std::nullopt;
                }
                ++files;
            }
            if (ec) {
                why = "cannot list " + Utf8Path(root) + " (" + ec.message() + ")";
                return std::nullopt;
            }
            if (!files) {
                why = "no files under " + Utf8Path(root);
                return std::nullopt;
            }
            // An unredirected result could be a future USVFS hiding the backing path; the copy stays safe.
            if (SamePath(backing, root)) {
                why = "files were not redirected";
                return std::nullopt;
            }
            return backing;
        }

        // Launch straight from the MO2 mod folder when one folder backs the whole tree.
        std::optional<std::filesystem::path> InPlace(const std::filesystem::path& root, std::string_view label)
        {
            const auto started = ::GetTickCount64();
            std::string why;
            const auto backing = BackingFolder(root, why);
            if (!backing) {
                REX::WARN("{} cannot launch from its MO2 mod folder ({}); using a local copy", label, why);
                return std::nullopt;
            }
            REX::INFO("{} launches in place from {} ({} ms)", label, Utf8Path(*backing), ::GetTickCount64() - started);
            return backing;
        }

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
        if (const auto backing = InPlace(dataDir / "bin" / "broker", "Browser broker"))
            return *backing / "osfui_browser_broker.exe";
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
			if (const auto backing = InPlace(source.parent_path(), "CEF runtime")) return *backing / source.filename();
			// Fallback: MO2's virtual DLL/resource tree must be materialized alongside the bootstrap.
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
        // The WebView2 helper copy is unused once the CEF edition is running.
        const auto webView2Copy = localRoot / "bin" / "osfui_webview2_host.exe";
        if (SamePath(hostExe, webView2Copy)) return;
        std::error_code ec;
        if (std::filesystem::remove(webView2Copy, ec)) REX::INFO("Removed unused WebView2 helper copy");
        if (ec) REX::WARN("Unused WebView2 helper copy not removed ({}); retrying next launch", ec.message());
    }

}
