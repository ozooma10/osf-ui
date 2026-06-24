#pragma once

namespace PrismaSF
{
	// Per-view permission grants. Everything defaults to "denied"; manifests
	// opt in explicitly. Permissions are recorded now and enforced at the
	// bridge/renderer boundary (filesystem/network have no implementation to
	// enforce yet — see docs/security-model.md).
	struct ViewPermissions
	{
		bool nativeBridge{ false };
		bool filesystem{ false };
		bool network{ false };
	};

	// Mirrors views/<id>/manifest.json.
	struct ViewManifest
	{
		std::string           id;
		std::string           title;
		std::string           entry{ "index.html" };
		std::uint32_t         width{ 1280 };
		std::uint32_t         height{ 720 };
		bool                  transparent{ true };
		std::int32_t          zorder{ 0 };         // compositing layer; lower draws beneath, higher on top
		bool                  interactive{ true };  // may receive input and become the active (focused) view
		ViewPermissions       permissions;
		std::filesystem::path rootDir;  // directory containing the manifest

		[[nodiscard]] std::filesystem::path EntryPath() const { return rootDir / entry; }

		// Parses a_path; returns std::nullopt and logs on any validation failure.
		static std::optional<ViewManifest> Load(const std::filesystem::path& a_path);
	};
}
