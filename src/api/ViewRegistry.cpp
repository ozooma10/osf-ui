#include "api/ViewRegistry.h"

namespace PrismaSF
{
	ViewRegistry::Record* ViewRegistry::Find(std::uint64_t a_handle)
	{
		const auto it = _records.find(a_handle);
		return it != _records.end() ? &it->second : nullptr;
	}

	const ViewRegistry::Record* ViewRegistry::Find(std::uint64_t a_handle) const
	{
		const auto it = _records.find(a_handle);
		return it != _records.end() ? &it->second : nullptr;
	}

	std::uint64_t ViewRegistry::Create(std::string a_htmlPath, DomReadyCb a_onDomReady)
	{
		std::scoped_lock lock(_mutex);
		const auto handle = _next++;

		int maxOrder = 0;
		for (const auto& [h, rec] : _records) {
			maxOrder = (std::max)(maxOrder, rec.order);
		}

		Record rec;
		rec.handle = handle;
		rec.internalId = "api/" + std::to_string(handle);
		rec.htmlPath = std::move(a_htmlPath);
		rec.order = _records.empty() ? 0 : maxOrder + 1;
		rec.onDomReady = std::move(a_onDomReady);
		_records.emplace(handle, std::move(rec));
		return handle;
	}

	std::string ViewRegistry::Remove(std::uint64_t a_handle)
	{
		std::scoped_lock lock(_mutex);
		const auto it = _records.find(a_handle);
		if (it == _records.end()) {
			return {};
		}
		auto id = it->second.internalId;
		_records.erase(it);
		return id;
	}

	bool ViewRegistry::Exists(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		return Find(a_handle) != nullptr;
	}

	std::string ViewRegistry::InternalId(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		const auto* rec = Find(a_handle);
		return rec ? rec->internalId : std::string{};
	}

	std::uint64_t ViewRegistry::HandleForInternalId(std::string_view a_id) const
	{
		std::scoped_lock lock(_mutex);
		for (const auto& [h, rec] : _records) {
			if (rec.internalId == a_id) {
				return h;
			}
		}
		return 0;
	}

	std::string ViewRegistry::HtmlPath(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		const auto* rec = Find(a_handle);
		return rec ? rec->htmlPath : std::string{};
	}

	void ViewRegistry::SetHidden(std::uint64_t a_handle, bool a_hidden)
	{
		std::scoped_lock lock(_mutex);
		if (auto* rec = Find(a_handle)) {
			rec->hidden = a_hidden;
		}
	}

	bool ViewRegistry::IsHidden(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		const auto* rec = Find(a_handle);
		return rec ? rec->hidden : true;  // unknown view reads as hidden
	}

	void ViewRegistry::SetOrder(std::uint64_t a_handle, int a_order)
	{
		std::scoped_lock lock(_mutex);
		if (auto* rec = Find(a_handle)) {
			rec->order = a_order;
		}
	}

	int ViewRegistry::GetOrder(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		const auto* rec = Find(a_handle);
		return rec ? rec->order : -1;
	}

	void ViewRegistry::SetScrollPixelSize(std::uint64_t a_handle, int a_px)
	{
		std::scoped_lock lock(_mutex);
		if (auto* rec = Find(a_handle)) {
			rec->scrollPx = a_px;
		}
	}

	int ViewRegistry::GetScrollPixelSize(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		const auto* rec = Find(a_handle);
		return rec ? rec->scrollPx : 0;
	}

	void ViewRegistry::SetFocused(std::uint64_t a_handle, bool a_focused, bool a_exclusive)
	{
		std::scoped_lock lock(_mutex);
		if (a_focused && a_exclusive) {
			for (auto& [h, rec] : _records) {
				rec.focused = false;
			}
		}
		if (auto* rec = Find(a_handle)) {
			rec->focused = a_focused;
		}
	}

	bool ViewRegistry::IsFocused(std::uint64_t a_handle) const
	{
		std::scoped_lock lock(_mutex);
		const auto* rec = Find(a_handle);
		return rec ? rec->focused : false;
	}

	bool ViewRegistry::AnyFocused() const
	{
		std::scoped_lock lock(_mutex);
		for (const auto& [h, rec] : _records) {
			if (rec.focused) {
				return true;
			}
		}
		return false;
	}

	void ViewRegistry::SetConsole(std::uint64_t a_handle, ConsoleCb a_cb)
	{
		std::scoped_lock lock(_mutex);
		if (auto* rec = Find(a_handle)) {
			rec->onConsole = std::move(a_cb);
		}
	}

	std::function<void()> ViewRegistry::DomReadyFor(std::string_view a_internalId) const
	{
		std::scoped_lock lock(_mutex);
		for (const auto& [h, rec] : _records) {
			if (rec.internalId == a_internalId) {
				if (!rec.onDomReady) {
					return {};
				}
				return [cb = rec.onDomReady, handle = rec.handle]() { cb(handle); };
			}
		}
		return {};
	}

	ViewRegistry::ConsoleCb ViewRegistry::ConsoleFor(std::string_view a_internalId) const
	{
		std::scoped_lock lock(_mutex);
		for (const auto& [h, rec] : _records) {
			if (rec.internalId == a_internalId) {
				return rec.onConsole;
			}
		}
		return {};
	}
}
