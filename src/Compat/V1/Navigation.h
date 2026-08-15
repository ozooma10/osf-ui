#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace OSFUI::Compat::V1
{
	inline constexpr std::string_view kRemovalVersion = "2.1.0";

	// URL-aware insertion for the temporary helper selector. The entry remains
	// otherwise byte-for-byte authored, including an existing query and fragment.
	[[nodiscard]] inline std::string WithLegacyApiQuery(std::string_view a_entry)
	{
		const auto hashAt = a_entry.find('#');
		const auto beforeHash = a_entry.substr(0, hashAt);
		const auto fragment = hashAt == std::string_view::npos ? std::string_view{} : a_entry.substr(hashAt);
		const auto queryAt = beforeHash.find('?');
		const auto path = beforeHash.substr(0, queryAt);
		const auto query = queryAt == std::string_view::npos ? std::string_view{} : beforeHash.substr(queryAt + 1);

		std::vector<std::string_view> kept;
		for (std::size_t start = 0; start <= query.size();) {
			const auto end = query.find('&', start);
			const auto part = query.substr(start,
				end == std::string_view::npos ? query.size() - start : end - start);
			const auto equals = part.find('=');
			if (!part.empty() && part.substr(0, equals) != "osfui-api") {
				kept.push_back(part);
			}
			if (end == std::string_view::npos) break;
			start = end + 1;
		}

		std::string out(path);
		out += '?';
		for (const auto part : kept) {
			out.append(part);
			out += '&';
		}
		out += "osfui-api=1";
		out.append(fragment);
		return out;
	}
}
