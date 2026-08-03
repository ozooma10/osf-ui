#pragma once

namespace OSFUI
{
	// The game's own key bindings, feeding the informational key-conflict view.
	// Starfield ships no controlmap data file (defaults live in the executable;
	// CommonLibSF has only RTTI ids for the live ControlMap singleton), so this
	// is a curated shipped defaults table (vanillakeys.json) overlaid by the
	// same controlmap text files the engine honors: a mod-provided
	// Data/Interface/Controls/PC/ControlMap.txt and the user's
	// Documents/My Games/Starfield/ControlMap_Custom.txt. Reading the live
	// singleton is the RE'd v2 that can replace this behind Bindings().
	//
	// The binding code space IS the controlmap's: DirectInput set-1 make codes
	// (0x80 | base for extended keys), the same space OSF UI key names resolve
	// to. A controlmap hex token is therefore consumed verbatim — no layout-
	// dependent translation anywhere in this pipeline.
	//
	// Host-testable: no Windows or game includes — the one platform-adjacent
	// fact (OSF UI key name -> scan code) is injected by the composition root.
	class VanillaKeys
	{
	public:
		struct Binding
		{
			std::string   event;  // engine controlmap event id ("QuickSave")
			std::string   label;  // human label for warnings ("Quicksave")
			std::uint32_t code;   // physical scan code (DIK convention)
		};

		// OSF UI key name -> scan code (Runtime wires input's ResolveKeyName).
		using NameResolver = std::function<std::uint32_t(std::string_view)>;

		// Parse the curated defaults table ({ "bindings": [ { event, label,
		// key } ] }); rows with an unresolvable/empty key name are skipped.
		// Returns false when the file is missing or not valid JSON — not
		// fatal, the conflict view just carries no vanilla data.
		bool LoadDefaults(const std::filesystem::path& a_path, const NameResolver& a_names);

		// Overlay one engine controlmap text file (tab-separated: event id,
		// keyboard DIK hex code(s), then mouse/gamepad/flags columns we
		// ignore). Only events present in the defaults table are touched;
		// first occurrence wins (gameplay context precedes menu contexts in
		// the engine files). The row's binding is replaced, or removed on
		// 0xff (unbound). Chorded specs ("0x1d+0x2e") are skipped — the
		// conflict domain is single physical keys. The hex tokens are already
		// in the binding code space, so they apply verbatim. A missing file is
		// a silent no-op. Returns the number of rows applied.
		std::size_t OverlayControlMap(const std::filesystem::path& a_path);

		// Format stamp for vanillakeys.json + vanillakeys.user.json (frozen
		// contract): a file declaring a newer version parses leniently with an
		// INFO; unknown keys warn (host-owned format — a typo, not skew).
		static constexpr std::int64_t kFormatVersion = 1;

		// The user's additive overlay,
		// Documents/My Games/Starfield/OSFUI/vanillakeys.user.json:
		//   { "formatVersion": 1,
		//     "add":      [ { "event", "label", "key" } ],   new rows
		//     "replace":  [ { "event", "key", "label"? } ],  rebind an existing row
		//     "suppress": [ "EventName", ... ] }             remove rows
		// Living under Documents, fixes survive OSF UI updates while untouched
		// shipped rows keep receiving upstream corrections. Rows with an
		// unresolvable key, and replace/suppress naming an unknown event, warn
		// (the shipped table is the event-id source of truth). A missing file is
		// a silent no-op. Returns the number of rows touched.
		// Apply after the controlmap overlays: the user's word is final.
		std::size_t OverlayUserFile(const std::filesystem::path& a_path, const NameResolver& a_names);

		// All rows; one unbound by an overlay (0xff) carries code == 0 —
		// consumers skip those. SettingsStore::SetVanillaKeys takes {label, code}.
		[[nodiscard]] const std::vector<Binding>& Bindings() const { return _bindings; }

	private:
		std::vector<Binding> _bindings;
	};
}
