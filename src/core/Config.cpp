#include "core/Config.h"

#include "runtime/Json.h"

namespace SWUI
{
	Config Config::Load(const std::filesystem::path& a_path)
	{
		Config config;

		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			REX::WARN("Config: {} not found; using built-in defaults", a_path.string());
			return config;
		}

		const auto json = Json::ParseFile(a_path);
		if (!json || !json->is_object()) {
			REX::ERROR("Config: {} is not a valid JSON object; using built-in defaults", a_path.string());
			return config;
		}

		config.enabled = Json::GetBool(*json, "enabled", config.enabled);
		config.toggleKey = Json::GetString(*json, "toggleKey", config.toggleKey);
		config.startVisible = Json::GetBool(*json, "startVisible", config.startVisible);
		config.renderer = Json::GetString(*json, "renderer", config.renderer);
		config.compositor = Json::GetString(*json, "compositor", config.compositor);
		config.inputSource = Json::GetString(*json, "inputSource", config.inputSource);
		config.view = Json::GetString(*json, "view", config.view);
		config.allowNetwork = Json::GetBool(*json, "allowNetwork", config.allowNetwork);
		config.devMode = Json::GetBool(*json, "devMode", config.devMode);

		if (config.allowNetwork) {
			// Nothing in this codebase performs network access; refuse the flag
			// loudly so nobody assumes it works.
			REX::WARN("Config: allowNetwork=true is not supported in this build; forcing false");
			config.allowNetwork = false;
		}

		REX::INFO("Config: loaded {} (renderer={}, compositor={}, inputSource={}, view={}, devMode={})",
			a_path.string(), config.renderer, config.compositor, config.inputSource, config.view, config.devMode);
		return config;
	}
}
