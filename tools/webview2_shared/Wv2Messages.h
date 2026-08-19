#pragma once


#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "Wv2Protocol.h"  // kDefaultLogicalHeight
#include "Core/Json.h"

namespace osfui::wv2::msg
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
		std::string   virtualHost{ "osfui.example" };
		std::uint32_t width{ 1 };
		std::uint32_t height{ 1 };
		std::string   userDataDir;
		bool          devMode{ false };
		bool          highRefreshCapture{ false };
		bool          hidden{ true };
		std::uint32_t adapterLuidLow{ 0 };
		std::uint32_t adapterLuidHigh{ 0 };

		static constexpr auto kFields = std::tuple{
			F("topLevelHwnd", &Init::topLevelHwnd),
			F("viewsPath", &Init::viewsPath),
			F("virtualHost", &Init::virtualHost),
			F("width", &Init::width),
			F("height", &Init::height),
			F("userDataDir", &Init::userDataDir),
			F("devMode", &Init::devMode),
			F("highRefreshCapture", &Init::highRefreshCapture),
			F("hidden", &Init::hidden),
			F("adapterLuidLow", &Init::adapterLuidLow),
			F("adapterLuidHigh", &Init::adapterLuidHigh),
		};
	};

	struct Navigate
	{
		static constexpr std::string_view kType = "navigate";
		std::string id;
		std::string entry{ "index.html" };
		bool        legacyApi{ false };
		std::uint32_t logicalHeight{ kDefaultLogicalHeight };

		static constexpr auto kFields = std::tuple{
			F("id", &Navigate::id),
			F("entry", &Navigate::entry),
			F("legacyApi", &Navigate::legacyApi),
			F("logicalHeight", &Navigate::logicalHeight),
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

#define OSFUI_WV2_VIEW_ONLY_MESSAGE(Name, TypeString)                        \
	struct Name                                                              \
	{                                                                        \
		static constexpr std::string_view kType = TypeString;                \
		std::string                       view;                              \
		static constexpr auto kFields = std::tuple{ F("view", &Name::view) }; \
	}

	OSFUI_WV2_VIEW_ONLY_MESSAGE(SetInputTarget, "setActive");
	OSFUI_WV2_VIEW_ONLY_MESSAGE(OpenDevTools, "openDevTools");
	OSFUI_WV2_VIEW_ONLY_MESSAGE(DestroyView, "destroyView");

#undef OSFUI_WV2_VIEW_ONLY_MESSAGE

	struct SetHidden
	{
		static constexpr std::string_view kType = "setHidden";
		std::string   view;
		bool          hidden{ true };
		std::uint64_t presentationEpoch{ 0 };

		static constexpr auto kFields = std::tuple{
			F("view", &SetHidden::view),
			F("hidden", &SetHidden::hidden),
			F("presentationEpoch", &SetHidden::presentationEpoch),
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

	struct Focus
	{
		static constexpr std::string_view kType = "focus";
		bool          focused{ false };
		std::uint64_t epoch{ 0 };
		std::string   view;

		static constexpr auto kFields = std::tuple{
			F("focused", &Focus::focused),
			F("epoch", &Focus::epoch),
			F("view", &Focus::view),
		};
	};

	/** Actual host focus, emitted after each request and WebView focus event. */
	struct FocusState
	{
		static constexpr std::string_view kType = "focusState";
		bool          focused{ false };
		std::uint64_t epoch{ 0 };
		std::uint64_t sequence{ 0 };
		std::string   view;

		static constexpr auto kFields = std::tuple{
			F("focused", &FocusState::focused),
			F("epoch", &FocusState::epoch),
			F("sequence", &FocusState::sequence),
			F("view", &FocusState::view),
		};
	};

	struct Mouse
	{
		static constexpr std::string_view kType = "mouse";
		// "move" | "button" | "wheel" | "physicalWheel"
		std::string  kind{ "move" };
		std::int32_t x{ 0 };
		std::int32_t y{ 0 };
		std::int32_t button{ 0 };
		bool         down{ false };
		std::int32_t wheel{ 0 };

		static constexpr auto kFields = std::tuple{
			F("kind", &Mouse::kind),
			F("x", &Mouse::x),
			F("y", &Mouse::y),
			F("button", &Mouse::button),
			F("down", &Mouse::down),
			F("wheel", &Mouse::wheel),
		};
	};

	struct Key
	{
		static constexpr std::string_view kType = "key";
		std::uint32_t vk{ 0 };
		bool          down{ false };

		static constexpr auto kFields = std::tuple{
			F("vk", &Key::vk),
			F("down", &Key::down),
		};
	};

	struct PostWeb
	{
		static constexpr std::string_view kType = "postWeb";
		std::string view;
		// An already-serialized bridge envelope, carried as an opaque string.
		std::string json;

		static constexpr auto kFields = std::tuple{
			F("view", &PostWeb::view),
			F("json", &PostWeb::json),
		};
	};

	struct AccelState
	{
		static constexpr std::string_view kType = "accelState";
		std::uint32_t toggleScan{ 0 };
		bool          captured{ false };
		bool          captureArmed{ false };
		std::uint32_t captureUpScan{ 0 };

		static constexpr auto kFields = std::tuple{
			F("toggleScan", &AccelState::toggleScan),
			F("captured", &AccelState::captured),
			F("captureArmed", &AccelState::captureArmed),
			F("captureUpScan", &AccelState::captureUpScan),
		};
	};

	struct Shutdown
	{
		static constexpr std::string_view kType = "shutdown";
		static constexpr auto             kFields = std::tuple{};
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
		std::string   hostVersion;
		std::string   runtimeVersion;
		std::uint32_t pid{ 0 };

		static constexpr auto kFields = std::tuple{
			F("protocolVersion", &Hello::protocolVersion),
			F("hostVersion", &Hello::hostVersion),
			F("runtimeVersion", &Hello::runtimeVersion),
			F("pid", &Hello::pid),
		};
	};

	struct Heartbeat
	{
		static constexpr std::string_view kType = "heartbeat";
		std::uint64_t tick{ 0 };

		static constexpr auto kFields = std::tuple{ F("tick", &Heartbeat::tick) };
	};

	struct Ready
	{
		static constexpr std::string_view kType = "ready";
		static constexpr auto             kFields = std::tuple{};
	};

	struct Textures
	{
		static constexpr std::string_view kType = "textures";
		std::uint32_t              width{ 0 };
		std::uint32_t              height{ 0 };
		std::vector<std::uint64_t> slots;
		std::uint64_t              produceFence{ 0 };
		std::vector<std::uint64_t> consumeFences;
		bool                       keyedMutex{ false };
		std::uint32_t              adapterLuidLow{ 0 };
		std::uint32_t              adapterLuidHigh{ 0 };

		static constexpr auto kFields = std::tuple{
			F("width", &Textures::width),
			F("height", &Textures::height),
			F("slots", &Textures::slots),
			F("produceFence", &Textures::produceFence),
			F("consumeFences", &Textures::consumeFences),
			F("keyedMutex", &Textures::keyedMutex),
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

		static constexpr auto kFields = std::tuple{
			F("slot", &Frame::slot),
			F("serial", &Frame::serial),
			F("width", &Frame::width),
			F("height", &Frame::height),
			F("presentationEpoch", &Frame::presentationEpoch),
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
		// Raw Runtime.consoleAPICalled params, carried opaquely.
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

	struct Accelerator
	{
		static constexpr std::string_view kType = "accelerator";
		std::uint32_t vk{ 0 };
		std::uint32_t scan{ 0 };
		bool          down{ false };

		static constexpr auto kFields = std::tuple{
			F("vk", &Accelerator::vk),
			F("scan", &Accelerator::scan),
			F("down", &Accelerator::down),
		};
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
