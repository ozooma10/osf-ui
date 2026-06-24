/*
 * PrismaUI SF — public modder API (Starfield edition).
 *
 * For modders: copy this file into your own SFSE plugin project to drive
 * PrismaUI SF views from native code.
 *
 * This interface is intentionally source-compatible with PrismaUI (Skyrim):
 * the namespace, the PrismaView handle, the IVPrismaUI1/IVPrismaUI2 vtables and
 * every method signature match PrismaUI_API.h byte-for-byte, so a consumer
 * written against Prisma UI ports to Starfield by changing little more than
 * which DLL RequestPluginAPI() resolves (here: "PrismaUI SF.dll").
 *
 * The interface is requested as a plain exported function (GetProcAddress),
 * NOT via SFSE messaging. Request it during or after SFSE's kPostLoad message
 * so the PrismaUI SF DLL is already loaded.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include <Windows.h>
#include <stdint.h>

typedef uint64_t PrismaView;

namespace PRISMA_UI_API
{
    constexpr const auto PrismaUIPluginName = "PrismaUI SF";

    // Available PrismaUI SF interface versions.
    enum class InterfaceVersion : uint8_t { V1, V2 };

    typedef void (*OnDomReadyCallback)(PrismaView view);
    typedef void (*JSCallback)(const char* result);
    typedef void (*JSListenerCallback)(const char* argument);

    // JavaScript console message severity level for use with RegisterConsoleCallback().
    enum class ConsoleMessageLevel : uint8_t { Log = 0, Warning, Error, Debug, Info };

    // Console message callback.
    typedef void (*ConsoleMessageCallback)(PrismaView view, ConsoleMessageLevel level, const char* message);

    // PrismaUI modder interface v1
    class IVPrismaUI1
    {
    protected:
        ~IVPrismaUI1() = default;

    public:
        // Create view. htmlPath is relative to <game>/Data/SFSE/Plugins/PrismaUI/views/.
        virtual PrismaView CreateView(const char* htmlPath,
                                      OnDomReadyCallback onDomReadyCallback = nullptr) noexcept = 0;

        // Send JS code to UI (arbitrary eval; optional callback receives the result string).
        virtual void Invoke(PrismaView view, const char* script, JSCallback callback = nullptr) noexcept = 0;

        // Call a JS function by name through the JS interop path (best performance, no eval).
        virtual void InteropCall(PrismaView view, const char* functionName, const char* argument) noexcept = 0;

        // Register a JS listener: exposes window.<functionName>(str) in the view; JS calls it
        // and the native callback fires (on the game main thread) with the string argument.
        virtual void RegisterJSListener(PrismaView view, const char* functionName,
                                        JSListenerCallback callback) noexcept = 0;

        // Returns true if view has input focus.
        virtual bool HasFocus(PrismaView view) noexcept = 0;

        // Set focus on view (captures keyboard/mouse). Optionally pause the game and/or skip the
        // engine focus menu. Returns true on success.
        virtual bool Focus(PrismaView view, bool pauseGame = false, bool disableFocusMenu = false) noexcept = 0;

        // Remove focus from view.
        virtual void Unfocus(PrismaView view) noexcept = 0;

        // Show a hidden view.
        virtual void Show(PrismaView view) noexcept = 0;

        // Hide a visible view.
        virtual void Hide(PrismaView view) noexcept = 0;

        // Returns true if view is hidden.
        virtual bool IsHidden(PrismaView view) noexcept = 0;

        // Get scroll size in pixels.
        virtual int GetScrollingPixelSize(PrismaView view) noexcept = 0;

        // Set scroll size in pixels.
        virtual void SetScrollingPixelSize(PrismaView view, int pixelSize) noexcept = 0;

        // Returns true if view exists.
        virtual bool IsValid(PrismaView view) noexcept = 0;

        // Completely destroy view.
        virtual void Destroy(PrismaView view) noexcept = 0;

        // Set view order (z-order among views).
        virtual void SetOrder(PrismaView view, int order) noexcept = 0;

        // Get view order.
        virtual int GetOrder(PrismaView view) noexcept = 0;

        // Create inspector view for debugging. (Not yet implemented in PrismaUI SF; no-op.)
        virtual void CreateInspectorView(PrismaView view) noexcept = 0;

        // Show or hide the inspector overlay. (Not yet implemented in PrismaUI SF; no-op.)
        virtual void SetInspectorVisibility(PrismaView view, bool visible) noexcept = 0;

        // Returns true if inspector is visible. (Not yet implemented in PrismaUI SF; returns false.)
        virtual bool IsInspectorVisible(PrismaView view) noexcept = 0;

        // Set inspector window position and size. (Not yet implemented in PrismaUI SF; no-op.)
        virtual void SetInspectorBounds(PrismaView view, float topLeftX, float topLeftY, unsigned int width,
                                        unsigned int height) noexcept = 0;

        // Returns true if any view has active focus.
        virtual bool HasAnyActiveFocus() noexcept = 0;
    };

    // PrismaUI modder interface v2 (extends v1)
    class IVPrismaUI2 : public IVPrismaUI1
    {
    protected:
        ~IVPrismaUI2() = default;

    public:
        // Register a callback to receive JavaScript console messages from a view.
        // Pass nullptr to unregister.
        virtual void RegisterConsoleCallback(PrismaView view, ConsoleMessageCallback callback) noexcept = 0;
    };

    // Maps interface types to InterfaceVersion enum values.
    // compile-time constraint -- only request interface versions that actually exist.
    template <typename T>
    struct InterfaceVersionMap;

    template <>
    struct InterfaceVersionMap<IVPrismaUI1>
    {
        static constexpr InterfaceVersion version = InterfaceVersion::V1;
    };

    template <>
    struct InterfaceVersionMap<IVPrismaUI2>
    {
        static constexpr InterfaceVersion version = InterfaceVersion::V2;
    };

    typedef void* (*RequestPluginAPIFunc)(InterfaceVersion interfaceVersion);

    /// Request the PrismaUI SF API interface.
    /// Recommended: send your request during or after SFSE's kPostLoad message so the DLL has
    /// already been loaded.
    [[nodiscard]] inline void* RequestPluginAPI(InterfaceVersion a_interfaceVersion = InterfaceVersion::V1)
    {
        // Explicit -W so this resolves correctly whether or not the consumer's
        // project defines UNICODE (GetModuleHandle would otherwise pick the ANSI
        // overload and reject the wide literal).
        auto pluginHandle = GetModuleHandleW(L"PrismaUI SF.dll");
        if (!pluginHandle) {
            return nullptr;
        }

        auto requestAPIFunction =
            reinterpret_cast<RequestPluginAPIFunc>(GetProcAddress(pluginHandle, "RequestPluginAPI"));

        if (requestAPIFunction) {
            return requestAPIFunction(a_interfaceVersion);
        }

        return nullptr;
    }

    /// Request a specific PrismaUI SF API interface version.
    /// Returns nullptr if the loaded PrismaUI SF DLL does not support the requested version.
    ///
    /// Usage:
    ///   auto* m_prisma   = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
    ///   auto* m_prismaV2 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
    template <typename T>
    [[nodiscard]] inline T* RequestPluginAPI()
    {
        return static_cast<T*>(RequestPluginAPI(InterfaceVersionMap<T>::version));
    }
}
