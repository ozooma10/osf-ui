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
		// Format stamp + typo diagnostics (item 8): host-shipped file, so an
		// unknown key is a typo, never version skew.
		if (const auto v = Json::GetInt(*json, "formatVersion", kFormatVersion); v > kFormatVersion) {
			REX::INFO("VanillaKeys: {} declares formatVersion {} (this build knows {}) — written for a newer OSF UI; unknown fields are ignored",
				a_path.string(), v, kFormatVersion);
		}
		Json::ReportUnknownKeys(*json, { "formatVersion", "bindings" }, "VanillaKeys: " + a_path.string(), /*a_warn=*/true);
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

	std::size_t VanillaKeys::OverlayUserFile(const std::filesystem::path& a_path, const NameResolver& a_names)
	{
		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			return 0;  // the overlay is optional
		}
		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::WARN("VanillaKeys: {} is not a valid JSON object; user overlay skipped", a_path.string());
			return 0;
		}
		const auto source = "VanillaKeys: " + a_path.string();
		if (const auto v = Json::GetInt(*json, "formatVersion", kFormatVersion); v > kFormatVersion) {
			REX::INFO("{} declares formatVersion {} (this build knows {}) — written for a newer OSF UI; unknown fields are ignored",
				source, v, kFormatVersion);
		}
		Json::ReportUnknownKeys(*json, { "formatVersion", "add", "replace", "suppress" }, source, /*a_warn=*/true);

		const auto findByEvent = [this](std::string_view a_event) {
			return std::ranges::find_if(_bindings, [&](const Binding& a_b) {
				return EqualsIgnoreCase(a_b.event, a_event);
			});
		};

		std::size_t count = 0;
		// suppress: ["EventName"] — remove the row entirely (it also leaves
		// the keybinds view's full map, which is the point).
		if (const auto it = json->find("suppress"); it != json->end() && it->is_array()) {
			for (const auto& entry : *it) {
				if (!entry.is_string()) {
					continue;
				}
				const auto event = entry.get<std::string>();
				if (const auto row = findByEvent(event); row != _bindings.end()) {
					_bindings.erase(row);
					++count;
				} else {
					REX::WARN("{} suppresses unknown event '{}' (typo? the shipped table names the event ids)", source, event.substr(0, 64));
				}
			}
		}
		// replace: [{event, key, label?}] — rebind an existing row.
		if (const auto it = json->find("replace"); it != json->end() && it->is_array()) {
			for (const auto& row : *it) {
				if (!row.is_object()) {
					continue;
				}
				Json::ReportUnknownKeys(row, { "event", "key", "label" }, source, /*a_warn=*/true);
				const auto event = Json::GetString(row, "event", "");
				const auto key = Json::GetString(row, "key", "");
				const auto target = findByEvent(event);
				if (target == _bindings.end()) {
					REX::WARN("{} replaces unknown event '{}' (typo? use \"add\" for new rows)", source, event.substr(0, 64));
					continue;
				}
				const auto vk = (!key.empty() && a_names) ? a_names(key) : 0;
				if (vk == 0) {
					REX::WARN("{} replace for '{}' names unresolvable key '{}'; row unchanged", source, event, key.substr(0, 32));
					continue;
				}
				target->vk = vk;
				if (const auto label = Json::GetString(row, "label", ""); !label.empty()) {
					target->label = label;
				}
				++count;
			}
		}
		// add: [{event, label, key}] — new rows (same shape as the shipped table).
		if (const auto it = json->find("add"); it != json->end() && it->is_array()) {
			for (const auto& row : *it) {
				if (!row.is_object()) {
					continue;
				}
				Json::ReportUnknownKeys(row, { "event", "label", "key" }, source, /*a_warn=*/true);
				Binding b{
					Json::GetString(row, "event", ""),
					Json::GetString(row, "label", ""),
					0,
				};
				const auto key = Json::GetString(row, "key", "");
				if (b.label.empty() || key.empty()) {
					REX::WARN("{} add row needs \"label\" and \"key\"; skipped", source);
					continue;
				}
				if (!b.event.empty() && findByEvent(b.event) != _bindings.end()) {
					REX::WARN("{} adds event '{}' which already exists (use \"replace\"); skipped", source, b.event.substr(0, 64));
					continue;
				}
				b.vk = a_names ? a_names(key) : 0;
				if (b.vk == 0) {
					REX::WARN("{} add row '{}' names unresolvable key '{}'; skipped", source, b.label.substr(0, 64), key.substr(0, 32));
					continue;
				}
				_bindings.push_back(std::move(b));
				++count;
			}
		}
		if (count) {
			REX::INFO("VanillaKeys: {} row(s) applied from user overlay {}", count, a_path.string());
		}
		return count;
	}
}
