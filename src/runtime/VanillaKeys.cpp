#include "runtime/VanillaKeys.h"

#include <cctype>
#include <charconv>
#include <fstream>

#include "core/Log.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs)
		{
			return std::ranges::equal(a_lhs, a_rhs, [](unsigned char l, unsigned char r) {
				return std::tolower(l) == std::tolower(r);
			});
		}

		std::string_view Trim(std::string_view a_s)
		{
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.front()))) {
				a_s.remove_prefix(1);
			}
			while (!a_s.empty() && std::isspace(static_cast<unsigned char>(a_s.back()))) {
				a_s.remove_suffix(1);
			}
			return a_s;
		}

		// "0x29" / "29" -> value; nullopt on anything else (labels, "!0", ...).
		std::optional<std::uint32_t> ParseHex(std::string_view a_token)
		{
			if (a_token.starts_with("0x") || a_token.starts_with("0X")) {
				a_token.remove_prefix(2);
			}
			if (a_token.empty() || a_token.size() > 8) {
				return std::nullopt;
			}
			std::uint32_t value = 0;
			const auto [ptr, ec] = std::from_chars(a_token.data(), a_token.data() + a_token.size(), value, 16);
			if (ec != std::errc{} || ptr != a_token.data() + a_token.size()) {
				return std::nullopt;
			}
			return value;
		}
	}

	bool VanillaKeys::LoadDefaults(const std::filesystem::path& a_path, const NameResolver& a_names)
	{
		_bindings.clear();
		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::WARN("VanillaKeys: {} missing or invalid; no vanilla key-conflict data", a_path.string());
			return false;
		}
		const auto it = json->find("bindings");
		if (it == json->end() || !it->is_array()) {
			REX::WARN("VanillaKeys: {} has no \"bindings\" array; no vanilla key-conflict data", a_path.string());
			return false;
		}
		for (const auto& row : *it) {
			if (!row.is_object()) {
				continue;
			}
			Binding b{
				Json::GetString(row, "event", ""),
				Json::GetString(row, "label", ""),
				0,
			};
			const auto key = Json::GetString(row, "key", "");
			if (b.label.empty() || key.empty()) {
				continue;  // curated table: a row without a label/key is noise
			}
			b.vk = a_names ? a_names(key) : 0;
			if (b.vk != 0) {
				_bindings.push_back(std::move(b));
			}
		}
		REX::INFO("VanillaKeys: {} binding(s) from {}", _bindings.size(), a_path.string());
		return true;
	}

	std::size_t VanillaKeys::OverlayControlMap(const std::filesystem::path& a_path, const ScanResolver& a_scan)
	{
		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			return 0;  // both overlay files are optional
		}
		std::ifstream file(a_path);
		if (!file) {
			REX::WARN("VanillaKeys: cannot open {}", a_path.string());
			return 0;
		}

		// First occurrence of an event wins: gameplay context precedes the
		// menu contexts in the engine's controlmap files, and our table is
		// context-less by design (menu keys would drown mod hotkeys in noise).
		std::vector<const Binding*> applied;
		std::size_t                 count = 0;
		std::string                 line;
		while (std::getline(file, line)) {
			const auto trimmed = Trim(line);
			if (trimmed.empty() || trimmed.starts_with("//")) {
				continue;
			}
			// Tab-separated: event id, keyboard spec, then mouse/gamepad/flag
			// columns this pass ignores.
			const auto eventEnd = trimmed.find_first_of(" \t");
			if (eventEnd == std::string_view::npos) {
				continue;
			}
			const auto event = trimmed.substr(0, eventEnd);
			const auto rest = Trim(trimmed.substr(eventEnd));
			const auto kbdEnd = rest.find_first_of(" \t");
			const auto kbdSpec = rest.substr(0, kbdEnd == std::string_view::npos ? rest.size() : kbdEnd);
			if (kbdSpec.empty()) {
				continue;
			}

			for (auto& binding : _bindings) {
				if (!EqualsIgnoreCase(binding.event, event) ||
					std::ranges::find(applied, &binding) != applied.end()) {
					continue;
				}
				applied.push_back(&binding);

				// Comma-separated alternatives; the first that resolves wins.
				// "0xff" = unbound; chords ("0x1d+0x2e") are out of scope —
				// the conflict domain is single physical keys.
				std::uint32_t vk = 0;
				bool          decided = false;
				std::string_view spec = kbdSpec;
				while (!spec.empty() && !decided) {
					const auto comma = spec.find(',');
					const auto token = Trim(spec.substr(0, comma));
					spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
					if (token.empty() || token.find('+') != std::string_view::npos) {
						continue;
					}
					if (EqualsIgnoreCase(token, "0xff")) {
						decided = true;  // explicitly unbound
						continue;
					}
					if (const auto sc = ParseHex(token)) {
						if (const auto mapped = a_scan ? a_scan(*sc) : 0; mapped != 0) {
							vk = mapped;
							decided = true;
						}
					}
				}
				if (decided && vk != binding.vk) {
					binding.vk = vk;
					++count;
				}
			}
		}
		if (count) {
			REX::INFO("VanillaKeys: {} binding(s) overridden by {}", count, a_path.string());
		}
		return count;
	}
}
