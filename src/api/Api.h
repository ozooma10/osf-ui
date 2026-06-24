#pragma once

#include <memory>

#include "api/PrismaUI_API.h"

namespace PrismaSF
{
	// Implements the public modder interface (PRISMA_UI_API::IVPrismaUI2) by
	// delegating to Runtime. Singleton with process lifetime (SFSE has no
	// shutdown callback). The exported RequestPluginAPI (Api.cpp) hands this out.
	//
	// Method order MUST match PrismaUI_API.h exactly for a correct vtable layout.
	class PluginAPI
	{
		using LatestInterface = PRISMA_UI_API::IVPrismaUI2;

	public:
		class PrismaUIInterface : public LatestInterface
		{
		public:
			static PrismaUIInterface* GetSingleton() noexcept
			{
				static PrismaUIInterface singleton;
				return std::addressof(singleton);
			}

			// --- IVPrismaUI1 ---
			PrismaView CreateView(const char* htmlPath,
				PRISMA_UI_API::OnDomReadyCallback onDomReadyCallback = nullptr) noexcept override;
			void Invoke(PrismaView view, const char* script,
				PRISMA_UI_API::JSCallback callback = nullptr) noexcept override;
			void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept override;
			void RegisterJSListener(PrismaView view, const char* functionName,
				PRISMA_UI_API::JSListenerCallback callback) noexcept override;
			bool HasFocus(PrismaView view) noexcept override;
			bool Focus(PrismaView view, bool pauseGame = false, bool disableFocusMenu = false) noexcept override;
			void Unfocus(PrismaView view) noexcept override;
			void Show(PrismaView view) noexcept override;
			void Hide(PrismaView view) noexcept override;
			bool IsHidden(PrismaView view) noexcept override;
			int  GetScrollingPixelSize(PrismaView view) noexcept override;
			void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept override;
			bool IsValid(PrismaView view) noexcept override;
			void Destroy(PrismaView view) noexcept override;
			void SetOrder(PrismaView view, int order) noexcept override;
			int  GetOrder(PrismaView view) noexcept override;
			void CreateInspectorView(PrismaView view) noexcept override;
			void SetInspectorVisibility(PrismaView view, bool visible) noexcept override;
			bool IsInspectorVisible(PrismaView view) noexcept override;
			void SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width,
				unsigned int height) noexcept override;
			bool HasAnyActiveFocus() noexcept override;

			// --- IVPrismaUI2 ---
			void RegisterConsoleCallback(PrismaView view,
				PRISMA_UI_API::ConsoleMessageCallback callback) noexcept override;

		private:
			PrismaUIInterface() noexcept = default;
			~PrismaUIInterface() noexcept = default;
		};
	};
}
