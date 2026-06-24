#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace PrismaSF
{
	// Handle table for views created through the public consumer API
	// (src/api/PrismaUI_API.h -> PRISMA_UI_API). Maps an opaque PrismaView handle
	// (uint64, never 0) to a record holding the view's logical state + the
	// consumer callbacks. All methods are thread-safe: the public API may be
	// called from any thread, and the renderer delivers callbacks on the game
	// thread, so both sides touch this table under the lock.
	//
	// The renderer keys its own ViewState by a string "internal id" ("api/<n>");
	// this table owns the handle <-> internal-id mapping.
	class ViewRegistry
	{
	public:
		using DomReadyCb = std::function<void(std::uint64_t a_handle)>;
		using ConsoleCb  = std::function<void(int a_level, std::string a_message)>;

		// Create a record for a new view. Returns the minted handle (never 0).
		// internalId becomes "api/<handle>"; order defaults to (current max + 1).
		std::uint64_t Create(std::string a_htmlPath, DomReadyCb a_onDomReady);

		// Remove the record; returns its internalId ("" if the handle is unknown).
		std::string Remove(std::uint64_t a_handle);

		[[nodiscard]] bool          Exists(std::uint64_t a_handle) const;
		[[nodiscard]] std::string   InternalId(std::uint64_t a_handle) const;        // "" if unknown
		[[nodiscard]] std::uint64_t HandleForInternalId(std::string_view a_id) const;  // 0 if unknown
		[[nodiscard]] std::string   HtmlPath(std::uint64_t a_handle) const;

		void               SetHidden(std::uint64_t a_handle, bool a_hidden);
		[[nodiscard]] bool IsHidden(std::uint64_t a_handle) const;

		void              SetOrder(std::uint64_t a_handle, int a_order);
		[[nodiscard]] int GetOrder(std::uint64_t a_handle) const;

		void              SetScrollPixelSize(std::uint64_t a_handle, int a_px);
		[[nodiscard]] int GetScrollPixelSize(std::uint64_t a_handle) const;

		// Focus: a_exclusive clears every other record's focus first.
		void               SetFocused(std::uint64_t a_handle, bool a_focused, bool a_exclusive);
		[[nodiscard]] bool IsFocused(std::uint64_t a_handle) const;
		[[nodiscard]] bool AnyFocused() const;

		void SetConsole(std::uint64_t a_handle, ConsoleCb a_cb);

		// Callback lookups by internal id (called when the renderer signals an
		// event on the game thread). Return a copy so the caller invokes it
		// outside the lock; empty std::function if none / unknown id.
		// Returns a nullary closure pre-bound to the view's handle (so the caller
		// need not know it), or empty if the id is unknown / has no callback.
		[[nodiscard]] std::function<void()> DomReadyFor(std::string_view a_internalId) const;
		[[nodiscard]] ConsoleCb             ConsoleFor(std::string_view a_internalId) const;

	private:
		struct Record
		{
			std::uint64_t handle{ 0 };
			std::string   internalId;
			std::string   htmlPath;
			int           order{ 0 };
			bool          hidden{ false };
			int           scrollPx{ 28 };  // matches PrismaUI's default scroll step
			bool          focused{ false };
			DomReadyCb    onDomReady;
			ConsoleCb     onConsole;
		};

		[[nodiscard]] Record*       Find(std::uint64_t a_handle);        // caller holds _mutex
		[[nodiscard]] const Record* Find(std::uint64_t a_handle) const;  // caller holds _mutex

		mutable std::mutex                        _mutex;
		std::uint64_t                             _next{ 1 };  // 0 reserved for "invalid"
		std::unordered_map<std::uint64_t, Record> _records;
	};
}
