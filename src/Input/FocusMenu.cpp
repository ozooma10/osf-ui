#include "Input/FocusMenu.h"

#include "RE/B/BSFixedString.h"
#include "RE/B/BSInputEventUser.h"
#include "RE/IDs_VTABLE.h"
#include "RE/I/IMenu.h"
#include "RE/S/ScaleformPtr.h"
#include "RE/U/UI.h"
#include "RE/U/UIMessageQueue.h"

#include "Core/Log.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace OSFUI
{
	namespace
	{
		// Set on the load thread (Register), read on the main thread (Open/Close from Runtime::Tick).
		std::atomic_bool g_registered{ false };

		// focus menu's +0x10 BSInputEventUser receiver is also engine-side gamepad capture gate.
		constexpr std::size_t kReceiverSlots = 10;
		std::atomic_bool      g_receiverBuilt{ false };
		void*                 g_receiverStore[kReceiverSlots + 1]{};
		void** const          g_receiverVtable = &g_receiverStore[1];
		std::atomic_bool      g_gamepadCapture{ false };

		bool Receiver_ShouldHandleEvent(void*, const RE::InputEvent*)
		{
			return true;
		}

		void ConsumeGamepadEvent(const void* a_event)
		{
			const_cast<RE::InputEvent*>(static_cast<const RE::InputEvent*>(a_event))->status = RE::InputEvent::Status::kStop;
		}

		void Receiver_OnThumbstick(void*, const void* a_event)
		{
			if (a_event && g_gamepadCapture.load(std::memory_order_relaxed)) {
				ConsumeGamepadEvent(a_event);
			}
		}

		void Receiver_OnCursorMove(void*, const void*) {}
		void Receiver_OnMouseMove(void*, const void*) {}

		void Receiver_OnCharacter(void*, const void* a_event)
		{
			if (a_event && Log::DebugEnabled()) {
				REX::DEBUG("FocusMenu: input char U+{:04X}", *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(a_event) + 0x28));
			}
		}

		void Receiver_OnButton(void*, const RE::ButtonEvent* a_event)
		{
			if (!a_event) {
				return;
			}
			if (a_event->deviceType == RE::InputEvent::DeviceType::kGamepad &&
				g_gamepadCapture.load(std::memory_order_relaxed)) {
				ConsumeGamepadEvent(a_event);
			}
			if (Log::DebugEnabled()) {
				REX::DEBUG("FocusMenu: input button dev={} id={:#x} value={:.2f} held={:.2f}", static_cast<std::uint32_t>(a_event->deviceType), a_event->idCode, a_event->value, a_event->heldDownSecs);
			}
		}

		void BuildReceiverVtable()
		{
			if (g_receiverBuilt.load(std::memory_order_acquire)) {
				return;
			}
			// RE::VTABLE::IMenu = { 475515 primary, 475519 (+0x50 event sink), 475517 (+0x10 BSInputEventUser) }. Preserve the RTTI COL at [-1].
			static REL::Relocation<std::uintptr_t> engineVtbl{ RE::VTABLE::IMenu[2] };
			const auto* src = reinterpret_cast<void* const*>(engineVtbl.address());
			g_receiverStore[0] = src[-1];
			for (std::size_t i = 0; i < kReceiverSlots; ++i) {
				g_receiverVtable[i] = src[i];
			}
			g_receiverVtable[1] = reinterpret_cast<void*>(&Receiver_ShouldHandleEvent);
			g_receiverVtable[4] = reinterpret_cast<void*>(&Receiver_OnThumbstick);
			g_receiverVtable[5] = reinterpret_cast<void*>(&Receiver_OnCursorMove);
			g_receiverVtable[6] = reinterpret_cast<void*>(&Receiver_OnMouseMove);
			g_receiverVtable[7] = reinterpret_cast<void*>(&Receiver_OnCharacter);
			g_receiverVtable[8] = reinterpret_cast<void*>(&Receiver_OnButton);
			g_receiverBuilt.store(true, std::memory_order_release);
		}

		void InstallInputReceiver(void* a_menuObj)
		{
			if (!a_menuObj) {
				return;
			}
			BuildReceiverVtable();
			*reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_menuObj) + 0x10) = &g_receiverVtable[0];
			REX::DEBUG("FocusMenu: gamepad capture gate installed on menu obj=0x{:016X} (+0x10 vtable copy)", reinterpret_cast<std::uintptr_t>(a_menuObj));
		}

		const RE::BSFixedString& MenuName()
		{
			static auto* const name = new RE::BSFixedString(FocusMenu::MENU_NAME.data());
			return *name;
		}

		// Construction constants, RE-verified on 1.16.244 (OSF RE ui.menu_flags).

		// RE 1.16.244 engine IMenu base initializer (ID 130615).
		constexpr std::uint64_t kID_IMenuBaseInit = 130615;  // 0x1425516b0

		// Engine owns this session-lifetime buffer; pin its refcount to avoid allocator mismatch.
		constexpr std::size_t kAllocSize = 0x200;
		constexpr std::size_t kVtblSlots = 32;  // covers IMenu vfuncs 0x00..0x1A

		// IMenu field offsets (mirror RE/I/IMenu.h static_asserts).
		constexpr std::size_t kOffRefCount = 0x008;  // u32 Scaleform refcount
		constexpr std::size_t kOffMovie    = 0x088;  // Scaleform::Ptr<Movie> uiMovie
		constexpr std::size_t kOffName     = 0x0B0;  // BSFixedString menuName  <- the crash field
		constexpr std::size_t kOffFlags    = 0x0C0;  // u32 RE::IMenu::Flag
		constexpr std::size_t kOffFlagsUpd = 0x0D2;  // bool flagsUpdated

		std::atomic_bool   g_vtblBuilt{ false };
		// Preserve the engine RTTI locator at vtable[-1] because admitted menus are dynamic_cast.
		void*              g_vtableStore[kVtblSlots + 1]{};
		void** const       g_vtable = &g_vtableStore[1];

		// Capture engine IMenu ProcessMessage from primary vtable slot 8 before patching.
		using ProcessMessageFn = std::int64_t (*)(void*, void*);
		ProcessMessageFn g_baseProcessMessage{ nullptr };

		// vtable thunks (MS x64 ABI matches the vtable thiscall)
		const char*   Thunk_GetName(void*) { return FocusMenu::MENU_NAME.data(); }
		const char*   Thunk_GetRootPath(void*) { return ""; }  // web-backed: no .swf root
		std::uint64_t Thunk_GetUnk05(void*) { return 0; }
		bool          Thunk_LoadMovie(void*, bool, bool) { return true; }  // success, no Scaleform movie

		// Handle kShow locally for movie-less admission; delegate kHide so the base removes the active entry.
		std::int64_t Thunk_ProcessMessage(void* a_this, void* a_msg)
		{
			// UIMessageData stores UI_MESSAGE_TYPE at +0x08; kShow=0, kUpdate=1, kHide=2.
			const auto type = a_msg
			                      ? *reinterpret_cast<const std::uint32_t*>(
			                            reinterpret_cast<const std::uint8_t*>(a_msg) + 0x08)
			                      : 0u;
			if (type == static_cast<std::uint32_t>(RE::UI_MESSAGE_TYPE::kShow)) {
				return 0;  // kHandled — the base's 1/kIgnore would refuse admission (Route A)
			}
			if (g_baseProcessMessage) {
				const auto ret = g_baseProcessMessage(a_this, a_msg);
				if (Log::DebugEnabled()) {
					REX::DEBUG("FocusMenu: ProcessMessage type={} -> engine base ret={}", type, ret);
				}
				return ret;
			}
			return 0;
		}
		// Force vf 0x0A true so the movie-less menu is admitted to UI+0x430.
		bool          Thunk_CanShow(void*) { return true; }

		// Patch a copied engine vtable because CommonLibSF's zero-ID IMenu thunks are invalid here.
		void BuildVtable()
		{
			if (g_vtblBuilt.load(std::memory_order_acquire)) {
				return;
			}
			static REL::Relocation<std::uintptr_t> engineVtbl{ RE::VTABLE::IMenu[0] };
			const auto* src = reinterpret_cast<void* const*>(engineVtbl.address());
			// Copy the engine RTTI locator into the leading vtable slot.
			g_vtableStore[0] = src[-1];
			for (std::size_t i = 0; i < kVtblSlots; ++i) {
				g_vtable[i] = src[i];
			}
			// Save base ProcessMessage for every non-show message.
			g_baseProcessMessage = reinterpret_cast<ProcessMessageFn>(src[8]);
			g_vtable[3] = reinterpret_cast<void*>(&Thunk_GetName);        // 03 GetName
			g_vtable[4] = reinterpret_cast<void*>(&Thunk_GetRootPath);    // 04 GetRootPath
			g_vtable[5] = reinterpret_cast<void*>(&Thunk_GetUnk05);       // 05 GetUnk05
			g_vtable[6] = reinterpret_cast<void*>(&Thunk_LoadMovie);      // 06 LoadMovie
			g_vtable[8] = reinterpret_cast<void*>(&Thunk_ProcessMessage); // 08 ProcessMessage (show->0, rest->engine base)
			g_vtable[10] = reinterpret_cast<void*>(&Thunk_CanShow);      // 0A stack-admission predicate
			g_vtblBuilt.store(true, std::memory_order_release);
		}
	}

	RE::Scaleform::Ptr<RE::IMenu>* FocusMenu::Creator(RE::Scaleform::Ptr<RE::IMenu>* a_out)
	{
		// Build an engine-initialized menu with a live +0xB0 name for keyed dispatch.
		BuildVtable();

		auto* obj = std::calloc(1, kAllocSize);
		if (!obj) {
			// Hand the engine a null Ptr; the open fails without crashing.
			*reinterpret_cast<void**>(a_out) = nullptr;
			return a_out;
		}

		// Engine base initialization installs vtables, refcount, and null uiMovie.
		static REL::Relocation<void (*)(void*)> baseInit{ REL::ID(kID_IMenuBaseInit) };
		baseInit(obj);

		auto* bytes = reinterpret_cast<std::uint8_t*>(obj);

		*reinterpret_cast<void**>(bytes + 0) = &g_vtable[0];

		// Keep uiMovie null; engine movie sites guard +0x88 before use.
		*reinterpret_cast<void**>(bytes + kOffMovie) = nullptr;

		// Construct the interned menuName in place at +0xB0.
		new (bytes + kOffName) RE::BSFixedString(MENU_NAME.data());

		// Set flags on the raw engine object; the C++ constructor never runs for it.
		constexpr std::uint32_t flags = 0;
		*reinterpret_cast<std::uint32_t*>(bytes + kOffFlags) = flags;
		*(bytes + kOffFlagsUpd) = 1;

		// Pin the session object so the engine never frees calloc memory through Scaleform.
		*reinterpret_cast<std::uint32_t*>(bytes + kOffRefCount) = 0x10000000;

		// Redirect only the observed BSInputEventUser slots after engine base initialization.
		InstallInputReceiver(obj);

		// Store directly into Scaleform::Ptr to avoid AddRef through the patched vtable.
		*reinterpret_cast<void**>(a_out) = obj;

		REX::DEBUG("FocusMenu: creator built engine-initialised menu obj=0x{:016X} flags=0x{:08X} (uiMovie=null, name@+0xB0 set)",
			reinterpret_cast<std::uintptr_t>(obj), static_cast<std::uint32_t>(flags));
		return a_out;
	}

	bool FocusMenu::Register()
	{
		if (g_registered.load(std::memory_order_acquire)) {
			return true;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("FocusMenu: RE::UI singleton null; cannot register (focus menu inert)");
			return false;
		}

		// Resolve all 1.16.244 construction addresses during registration and fail closed after patches.
		BuildVtable();

		if (ui->IsMenuRegistered(MenuName())) {
			REX::DEBUG("FocusMenu: '{}' already registered", MENU_NAME);
			g_registered.store(true, std::memory_order_release);
			return true;
		}
		ui->RegisterMenu(MENU_NAME.data(), &FocusMenu::Creator);
		g_registered.store(true, std::memory_order_release);
		REX::INFO("FocusMenu: registered '{}' (hardened creator: engine base-init + engine vtable copy + "
				  "interned +0xB0 name; opens only when the overlay does). RE-verified 1.16.244.",
			MENU_NAME);
		return true;
	}

	bool FocusMenu::IsRegistered()
	{
		return g_registered.load(std::memory_order_acquire);
	}

	bool FocusMenu::IsOpenInEngine()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return false;
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return false;
		}
		// Walk UI+0x430 and compare the creator-interned name at +0xB0.
		for (const auto& menu : ui->menuArray) {
			if (menu && menu->menuName == MENU_NAME) {
				return true;
			}
		}
		return false;
	}

	void FocusMenu::SetGamepadCapture(bool a_capture)
	{
		if (g_gamepadCapture.exchange(a_capture, std::memory_order_relaxed) != a_capture) {
			REX::DEBUG("FocusMenu: gamepad capture gate {} (Starfield {} controller input)", a_capture ? "ON" : "off", a_capture ? "no longer receives" : "receives");
		}
	}

	void FocusMenu::Open()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return;
		}
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MenuName(), RE::UI_MESSAGE_TYPE::kShow);
			REX::DEBUG("FocusMenu: open requested ('{}' kShow)", MENU_NAME);
		} else {
			REX::WARN("FocusMenu: UIMessageQueue singleton null; cannot open");
		}
	}

	void FocusMenu::Close()
	{
		if (!g_registered.load(std::memory_order_acquire)) {
			return;
		}
		if (auto* queue = RE::UIMessageQueue::GetSingleton()) {
			queue->AddMessage(MenuName(), RE::UI_MESSAGE_TYPE::kHide);
			REX::DEBUG("FocusMenu: close requested ('{}' kHide)", MENU_NAME);
		}
	}

}
