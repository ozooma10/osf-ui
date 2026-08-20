#pragma once

// Minimal consumer declaration for Carbon UI public API v1. The ABI and field
// order mirror the provider's official CarbonUI_API.h. This benchmark-owned
// declaration avoids making the build depend on a locally cloned Carbon tree.
// Upstream: https://github.com/CarbonNode/CarbonUI/blob/main/include/CarbonUI_API.h

#include <cstdint>

namespace CarbonUI
{
	using ViewHandle = std::uint32_t;
	inline constexpr ViewHandle kInvalidView = 0;
	inline constexpr std::uint32_t kAPIVersion = 1;
	inline constexpr const char* kProviderPluginName = "CarbonUI";
	inline constexpr std::uint32_t kMessageType_RequestAPI = 0x43554931u;

	using JsCallback = const char* (*)(const char*, void*);

	struct ViewDesc
	{
		std::uint32_t structSize;
		std::uint32_t reserved0;
		const char* url;
		const char* html;
		std::uint32_t width;
		std::uint32_t height;
		std::int32_t x;
		std::int32_t y;
		std::int32_t z;
		std::uint8_t transparent;
		std::uint8_t visible;
		std::uint8_t focusable;
		std::uint8_t reserved1;
		const char* ownerName;
	};

	struct IAPI
	{
		void* ctx;
		std::uint32_t (*GetVersion)(void*);
		std::uint32_t (*GetProviderVersion)(void*);
		ViewHandle (*CreateView)(void*, const ViewDesc*);
		void (*DestroyView)(void*, ViewHandle);
		bool (*IsViewValid)(void*, ViewHandle);
		bool (*LoadURL)(void*, ViewHandle, const char*);
		bool (*LoadHTML)(void*, ViewHandle, const char*);
		bool (*ShowView)(void*, ViewHandle, bool);
		bool (*FocusView)(void*, ViewHandle, bool);
		bool (*SetViewRect)(void*, ViewHandle, std::int32_t, std::int32_t, std::uint32_t, std::uint32_t);
		bool (*SetViewZ)(void*, ViewHandle, std::int32_t);
		bool (*Invoke)(void*, ViewHandle, const char*);
		bool (*RegisterListener)(void*, ViewHandle, const char*, JsCallback, void*);
		bool (*UnregisterListener)(void*, ViewHandle, const char*);
		void (*SetFreeze)(void*, bool);
		bool (*IsAnyViewOpen)(void*);
	};

	struct RequestAPIMessage
	{
		std::uint32_t structSize;
		std::uint32_t requestedVersion;
		IAPI* api;
		std::uint32_t providerVersion;
		std::uint32_t reserved;
	};

	template <class MessagingT>
	inline const IAPI* RequestCarbonUIAPI(const MessagingT* a_messaging)
	{
		if (!a_messaging) {
			return nullptr;
		}
		RequestAPIMessage request{};
		request.structSize = sizeof(request);
		request.requestedVersion = kAPIVersion;
		a_messaging->Dispatch(kMessageType_RequestAPI, &request, sizeof(request), kProviderPluginName);
		return request.api;
	}
}
