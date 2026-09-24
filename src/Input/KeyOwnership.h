#pragma once

#include <array>
#include <cstdint>

namespace OSFUI
{
	// Window-thread ownership of a complete key press, across capture changes.
	class KeyOwnership
	{
	public:
		bool Consume(std::uint32_t a_vk, bool a_down, bool a_capture)
		{
			if (a_vk >= _owners.size()) return a_capture;
			auto& owner = _owners[a_vk];
			if (a_down) {
				if (owner == Owner::None) owner = a_capture ? Owner::Overlay : Owner::Game;
				return a_capture || owner == Owner::Overlay;
			}
			const bool consume = owner == Owner::Overlay || (owner == Owner::None && a_capture);
			owner = Owner::None;
			return consume;
		}

		void Reset() { _owners.fill(Owner::None); }

	private:
		enum class Owner : std::uint8_t { None, Game, Overlay };
		std::array<Owner, 256> _owners{};
	};
}
