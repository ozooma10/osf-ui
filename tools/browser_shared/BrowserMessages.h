#pragma once


#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "BrowserBackend.h"
#include "BrowserProtocol.h"  // kDefaultLogicalHeight
#include "Core/Json.h"

namespace osfui::browser::msg
{
	// One wire field: the JSON key and the member it binds to.
	template <class S, class M>
	struct Field
	{
		std::string_view key;
		M S::*           member;
	};

	template <class S, class M>
	[[nodiscard]] constexpr Field<S, M> F(std::string_view a_key, M S::*a_member)
	{
		return { a_key, a_member };
	}

	namespace detail
	{
		template <class T>
		void ReadInto(T& a_out, const nlohmann::json& a_msg, std::string_view a_key)
		{
			if constexpr (std::is_same_v<T, std::vector<std::uint64_t>>) {
				a_out.clear();
				if (const auto* array = OSFUI::Json::GetArray(a_msg, a_key)) {
					a_out.reserve(array->size());
					for (const auto& element : *array) {
						if (element.is_number_integer()) {
							a_out.push_back(element.get<std::uint64_t>());
						}
					}
				}
            } else if constexpr (std::is_same_v<T, nlohmann::json>) {
                const auto it=a_msg.find(a_key);
                if (it!=a_msg.end()) a_out=*it;
            } else {
				a_out = OSFUI::Json::Get(a_msg, a_key, a_out);
			}
		}
	}

	// Serialize a message, stamping its `type`.
	template <class S>
	[[nodiscard]] nlohmann::json ToJson(const S& a_msg)
	{
		nlohmann::json out{ { "type", std::string(S::kType) } };
		std::apply([&](const auto&... a_field) {
			((out[std::string(a_field.key)] = a_msg.*(a_field.member)), ...);
		},
			S::kFields);
		return out;
	}

	template <class S>
	[[nodiscard]] S FromJson(const nlohmann::json& a_msg)
	{
		S out{};
		std::apply([&](const auto&... a_field) {
			(detail::ReadInto(out.*(a_field.member), a_msg, a_field.key), ...);
		},
			S::kFields);
		return out;
	}


	struct Init
	{
		static constexpr std::string_view kType = "init";
		std::uint64_t topLevelHwnd{ 0 };
		std::string   viewsPath;
		std::string   legacyViewsPath;
		std::uint32_t width{ 1 };
		std::uint32_t height{ 1 };
		std::string   userDataDir;
		bool          devMode{ false };
		std::string   language;  // game language code ("en", "ptbr"); empty keeps the browser default
		std::uint32_t adapterLuidLow{ 0 };
		std::uint32_t adapterLuidHigh{ 0 };
		bool          remoteMode{ false };

		static constexpr auto kFields = std::tuple{
			F("topLevelHwnd", &Init::topLevelHwnd),
			F("viewsPath", &Init::viewsPath),
			F("legacyViewsPath", &Init::legacyViewsPath),
			F("width", &Init::width),
			F("height", &Init::height),
			F("userDataDir", &Init::userDataDir),
			F("devMode", &Init::devMode),
			F("language", &Init::language),
			F("adapterLuidLow", &Init::adapterLuidLow),
			F("adapterLuidHigh", &Init::adapterLuidHigh),
			F("remoteMode", &Init::remoteMode),
		};
	};

	struct Navigate
	{
		static constexpr std::string_view kType = "navigate";
		std::string id;
		std::string entry{ "index.html" };
		std::uint32_t logicalHeight{ kDefaultLogicalHeight };
		bool legacy{};
		bool remoteContent{};
		std::string url; // Absolute URL for an untrusted content view; never a local entry.

		static constexpr auto kFields = std::tuple{
			F("id", &Navigate::id),
			F("entry", &Navigate::entry),
			F("logicalHeight", &Navigate::logicalHeight),
			F("legacy", &Navigate::legacy),
			F("remoteContent", &Navigate::remoteContent),
			F("url", &Navigate::url),
		};
	};

	struct Resize
	{
		static constexpr std::string_view kType = "resize";
		std::uint32_t width{ 1 };
		std::uint32_t height{ 1 };

		static constexpr auto kFields = std::tuple{
			F("width", &Resize::width),
			F("height", &Resize::height),
		};
	};

	struct BrowserLayout
	{
		static constexpr std::string_view kType = "browserLayout";
		std::string chrome, content;
		std::uint32_t chromeHeight{56};
		bool fullscreen{}, landing{true};
		std::uint64_t presentationEpoch{};
		static constexpr auto kFields = std::tuple{
			F("chrome", &BrowserLayout::chrome), F("content", &BrowserLayout::content),
			F("chromeHeight", &BrowserLayout::chromeHeight), F("fullscreen", &BrowserLayout::fullscreen),
			F("landing", &BrowserLayout::landing), F("presentationEpoch", &BrowserLayout::presentationEpoch)
		};
	};
    struct BrowserAudio
    {
        static constexpr std::string_view kType = "browserAudio";
        nlohmann::json scene;
        std::uint64_t expiresMs{};
        static constexpr auto kFields = std::tuple{F("scene",&BrowserAudio::scene),F("expiresMs",&BrowserAudio::expiresMs)};
    };
	struct BrowserCommand
	{
		static constexpr std::string_view kType = "browserCommand";
		std::string view, action, url;
		bool value{};
		std::uint64_t token{};
		static constexpr auto kFields = std::tuple{ F("view", &BrowserCommand::view),
			F("action", &BrowserCommand::action), F("url", &BrowserCommand::url),
			F("value", &BrowserCommand::value), F("token", &BrowserCommand::token) };
	};
	struct BrowserResult
	{
		static constexpr std::string_view kType = "browserResult";
		std::uint64_t token{};
		bool success{};
		bool restartRequired{};
		std::string error;
		static constexpr auto kFields = std::tuple{ F("token", &BrowserResult::token),
			F("success", &BrowserResult::success), F("restartRequired", &BrowserResult::restartRequired), F("error", &BrowserResult::error) };
	};
	struct BrowserState
	{
		static constexpr std::string_view kType = "browserState";
		std::string view, url, title;
		bool canGoBack{}, canGoForward{}, loading{}, secure{}, audible{}, muted{};
		bool audibleKnown{true};
        nlohmann::json audio; // CEF diagnostics, populated only in developer mode.
		static constexpr auto kFields = std::tuple{ F("view", &BrowserState::view), F("url", &BrowserState::url),
			F("title", &BrowserState::title), F("canGoBack", &BrowserState::canGoBack),
			F("canGoForward", &BrowserState::canGoForward), F("loading", &BrowserState::loading),
			F("secure", &BrowserState::secure), F("audible", &BrowserState::audible), F("muted", &BrowserState::muted),
			F("audibleKnown", &BrowserState::audibleKnown), F("audio", &BrowserState::audio) };
	};

	struct Viewport
	{
		static constexpr std::string_view kType = "viewport";
		std::uint32_t width{ 1 };
		std::uint32_t height{ 1 };
		std::uint64_t presentationEpoch{ 0 };

		static constexpr auto kFields = std::tuple{
			F("width", &Viewport::width),
			F("height", &Viewport::height),
			F("presentationEpoch", &Viewport::presentationEpoch),
		};
	};

	struct PointerInput
	{
		static constexpr std::string_view kType = "pointerInput";
		bool enabled{ false };

		static constexpr auto kFields = std::tuple{
			F("enabled", &PointerInput::enabled),
		};
	};

#define OSFUI_BROWSER_VIEW_ONLY_MESSAGE(Name, TypeString)                        \
	struct Name                                                              \
	{                                                                        \
		static constexpr std::string_view kType = TypeString;                \
		std::string                       view;                              \
		static constexpr auto kFields = std::tuple{ F("view", &Name::view) }; \
	}

	OSFUI_BROWSER_VIEW_ONLY_MESSAGE(SetInputTarget, "setInputTarget");
	OSFUI_BROWSER_VIEW_ONLY_MESSAGE(OpenDevTools, "openDevTools");
	OSFUI_BROWSER_VIEW_ONLY_MESSAGE(DestroyView, "destroyView");

#undef OSFUI_BROWSER_VIEW_ONLY_MESSAGE

	// Let an open select handle Back first. Return unhandled requests with the menu epoch.
	struct Back
	{
		static constexpr std::string_view kType = "back";
		std::string view;
		std::uint64_t presentationEpoch{};
		static constexpr auto kFields = std::tuple{
			F("view", &Back::view), F("presentationEpoch", &Back::presentationEpoch)
		};
	};
	struct BackUnhandled
	{
		static constexpr std::string_view kType = "backUnhandled";
		std::string view;
		std::uint64_t presentationEpoch{};
		static constexpr auto kFields = std::tuple{
			F("view", &BackUnhandled::view), F("presentationEpoch", &BackUnhandled::presentationEpoch)
		};
	};

	struct SetHidden
	{
		static constexpr std::string_view kType = "setHidden";
		std::string   view;
		bool          hidden{ true };
		std::uint64_t presentationEpoch{ 0 };
		bool settleColdOpen{ false }; // first launcher open after navigation gets a short capture grace period

		static constexpr auto kFields = std::tuple{
			F("view", &SetHidden::view),
			F("hidden", &SetHidden::hidden),
			F("presentationEpoch", &SetHidden::presentationEpoch),
			F("settleColdOpen", &SetHidden::settleColdOpen),
		};
	};

	struct SetOrder
	{
		static constexpr std::string_view kType = "setOrder";
		std::string  view;
		std::int32_t order{ 0 };

		static constexpr auto kFields = std::tuple{
			F("view", &SetOrder::view),
			F("order", &SetOrder::order),
		};
	};

	// Applies to the current input target (setInputTarget).
	struct Focus
	{
		static constexpr std::string_view kType = "focus";
		bool focused{ false };

		static constexpr auto kFields = std::tuple{ F("focused", &Focus::focused) };
	};

	struct Mouse
	{
		static constexpr std::string_view kType = "mouse";
		// "move" | "button" | "wheel"
		std::string  kind{ "move" };
		std::int32_t x{ 0 };
		std::int32_t y{ 0 };
		std::int32_t button{ 0 };
		bool         down{ false };
		std::int32_t wheel{ 0 };
		std::uint32_t modifiers{ 0 }; // Win32 MK_SHIFT/MK_CONTROL, sampled by the game

		static constexpr auto kFields = std::tuple{
			F("kind", &Mouse::kind),
			F("x", &Mouse::x),
			F("y", &Mouse::y),
			F("button", &Mouse::button),
			F("down", &Mouse::down),
			F("wheel", &Mouse::wheel),
			F("modifiers", &Mouse::modifiers),
		};
	};

	// Keyboard input delivered through CDP: physical keys and gamepad navigation keys.
	struct Keyboard
	{
		static constexpr std::string_view kType = "keyboard";
		std::uint32_t vk{ 0 }, modifiers{ 0 }, location{ 0 };
		bool down{ false }, repeat{ false }, system{ false }, keypad{ false };
		std::string key, code;
		static constexpr auto kFields = std::tuple{
			F("vk", &Keyboard::vk), F("modifiers", &Keyboard::modifiers),
			F("location", &Keyboard::location), F("down", &Keyboard::down),
			F("repeat", &Keyboard::repeat), F("system", &Keyboard::system),
			F("keypad", &Keyboard::keypad), F("key", &Keyboard::key), F("code", &Keyboard::code),
		};
	};

	struct TextInput
	{
		static constexpr std::string_view kType = "textInput";
		std::string text;
		// char = translated WM_CHAR; composition/commit/cancel = Windows IME.
		std::string kind{ "char" };
		std::uint32_t cursor{ 0 };
		static constexpr auto kFields = std::tuple{
			F("text", &TextInput::text), F("kind", &TextInput::kind), F("cursor", &TextInput::cursor),
		};
	};

	struct WindowActive
	{
		static constexpr std::string_view kType = "windowActive";
		bool active{ false };
		static constexpr auto kFields = std::tuple{ F("active", &WindowActive::active) };
	};

	// Sent when the game recreates its window (window-mode change); supersedes Init::topLevelHwnd.
	struct GameWindow
	{
		static constexpr std::string_view kType = "gameWindow";
		std::uint64_t hwnd{ 0 };
		static constexpr auto kFields = std::tuple{ F("hwnd", &GameWindow::hwnd) };
	};

	struct PostWeb
	{
		static constexpr std::string_view kType = "postWeb";
		std::string view;
		// Serialized bridge JSON; pass through without parsing.
		std::string json;

		static constexpr auto kFields = std::tuple{
			F("view", &PostWeb::view),
			F("json", &PostWeb::json),
		};
	};

	struct FrameAck
	{
		static constexpr std::string_view kType = "frameAck";
		std::uint32_t slot{ 0 };
		std::uint64_t serial{ 0 };

		static constexpr auto kFields = std::tuple{
			F("slot", &FrameAck::slot),
			F("serial", &FrameAck::serial),
		};
	};

	struct Hello
	{
		static constexpr std::string_view kType = "hello";
		std::uint32_t protocolVersion{ 0 };
		std::string   runtimeVersion;
        std::string backend;
        std::uint32_t capabilities{};

		static constexpr auto kFields = std::tuple{
			F("protocolVersion", &Hello::protocolVersion),
			F("runtimeVersion", &Hello::runtimeVersion),
            F("backend", &Hello::backend),
            F("capabilities", &Hello::capabilities),
		};
	};

    inline bool IsCompatibleHello(const Hello& hello, Backend expected)
    {
        return hello.protocolVersion == kBrowserHostProtocolVersion &&
            hello.backend == BackendName(expected) &&
            (hello.capabilities & kRequiredHostCapabilities) == kRequiredHostCapabilities;
    }

	struct Heartbeat
	{
		static constexpr std::string_view kType = "heartbeat";
		static constexpr auto             kFields = std::tuple{};
	};

	struct Textures
	{
		static constexpr std::string_view kType = "textures";
		std::uint32_t              width{ 0 };
		std::uint32_t              height{ 0 };
		std::vector<std::uint64_t> slots;
		std::uint64_t              produceFence{ 0 };
		std::uint32_t              adapterLuidLow{ 0 };
		std::uint32_t              adapterLuidHigh{ 0 };

		static constexpr auto kFields = std::tuple{
			F("width", &Textures::width),
			F("height", &Textures::height),
			F("slots", &Textures::slots),
			F("produceFence", &Textures::produceFence),
			F("adapterLuidLow", &Textures::adapterLuidLow),
			F("adapterLuidHigh", &Textures::adapterLuidHigh),
		};
	};

	struct Frame
	{
		static constexpr std::string_view kType = "frame";
		std::uint32_t slot{ 0 };
		std::uint64_t serial{ 0 };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint64_t presentationEpoch{ 0 };
		bool complete{ true }; // partial composites may update an existing scene, never finish a reveal

		static constexpr auto kFields = std::tuple{
			F("slot", &Frame::slot),
			F("serial", &Frame::serial),
			F("width", &Frame::width),
			F("height", &Frame::height),
			F("presentationEpoch", &Frame::presentationEpoch),
			F("complete", &Frame::complete),
		};
	};

	struct LoadEvent
	{
		static constexpr std::string_view kType = "loadEvent";
		std::string  view;
		bool         failed{ false };
		std::string  url;
		std::string  description;
		std::int32_t code{ 0 };

		static constexpr auto kFields = std::tuple{
			F("view", &LoadEvent::view),
			F("failed", &LoadEvent::failed),
			F("url", &LoadEvent::url),
			F("description", &LoadEvent::description),
			F("code", &LoadEvent::code),
		};
	};

	struct Fatal
	{
		static constexpr std::string_view kType = "fatal";
		std::string   stage{ "renderer" };
		std::string   view;
		std::string   description{ "terminal renderer failure" };
		std::uint32_t code{ 0 };

		static constexpr auto kFields = std::tuple{
			F("stage", &Fatal::stage),
			F("view", &Fatal::view),
			F("description", &Fatal::description),
			F("code", &Fatal::code),
		};
	};

	struct WebMessage
	{
		static constexpr std::string_view kType = "webMessage";
		std::string view;
		std::string json;

		static constexpr auto kFields = std::tuple{
			F("view", &WebMessage::view),
			F("json", &WebMessage::json),
		};
	};

	struct Console
	{
		static constexpr std::string_view kType = "console";
		std::string view;
		// Raw Runtime.consoleAPICalled params; pass through unchanged.
		std::string json;

		static constexpr auto kFields = std::tuple{
			F("view", &Console::view),
			F("json", &Console::json),
		};
	};

	struct Cursor
	{
		static constexpr std::string_view kType = "cursor";
		// Input-target view only; Win32 cursor id, 0 = hidden.
		std::uint32_t id{ 0 };

		static constexpr auto kFields = std::tuple{ F("id", &Cursor::id) };
	};

	struct Log
	{
		static constexpr std::string_view kType = "log";
		std::int32_t level{ 0 };
		std::string  text;

		static constexpr auto kFields = std::tuple{
			F("level", &Log::level),
			F("text", &Log::text),
		};
	};

	struct Bye
	{
		static constexpr std::string_view kType = "bye";
		std::string reason;

		static constexpr auto kFields = std::tuple{ F("reason", &Bye::reason) };
	};
}
