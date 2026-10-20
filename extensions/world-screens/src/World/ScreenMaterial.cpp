#include "World/ScreenMaterial.h"
#include "RE/N/NiNode.h"
#include "REX/FModule.h"

namespace OSFUI::ScreenMaterial
{
	namespace
	{
		struct ResourceKey { std::uint32_t file{}, extension{}, directory{}; };
		static_assert(sizeof(ResourceKey) == 12);
		using MakeKey = void (*)(ResourceKey*, const char*);
		using FindMaterial = std::uint32_t* (*)(std::uint32_t*, const ResourceKey*);
		using SetMaterial = void (*)(void*, std::uint32_t);

		void* FindScreen(RE::NiAVObject* a_object, unsigned& a_count, unsigned a_depth = 0)
		{
			if (!a_object || ++a_count > 128 || a_depth > 16) return nullptr;
			const auto name = std::string_view(a_object->name.c_str());
			if (name == "screens025:0" || name == "OSFUIScreen") return a_object->GetAsBSGeometry();
			if (auto* node = a_object->GetAsNiNode(); node && node->children && node->childrenSize <= node->childrenMaxSize && node->childrenSize <= 128) {
				for (std::uint16_t i = 0; i < node->childrenSize; ++i) {
					if (auto* screen = FindScreen(node->children[i], a_count, a_depth + 1)) return screen;
				}
			}
			return nullptr;
		}
	}

	bool Supported()
	{
		const auto module = REX::FModule::GetExecutingModule();
		return module.GetFileVersion() == REL::Version(1, 16, 244, 0) &&
			REL::Relocation<MakeKey>{ REL::ID(137049) }.address() == module.GetBaseAddress() + 0x27D7580 &&
			REL::Relocation<FindMaterial>{ REL::ID(129165) }.address() == module.GetBaseAddress() + 0x24D47C0 &&
			REL::Relocation<SetMaterial>{ REL::ID(147289) }.address() == module.GetBaseAddress() + 0x2BDAF10;
	}

	bool FullUV(RE::NiAVObject* a_root)
	{
		unsigned visited{};
		auto* geometry = static_cast<RE::NiAVObject*>(FindScreen(a_root, visited));
		return geometry && std::string_view(geometry->name.c_str()) == "OSFUIScreen";
	}

	bool Apply(RE::NiAVObject* a_root, std::string_view a_materialPath)
	{
		if (!Supported() || !a_materialPath.starts_with("materials/") || a_materialPath.size() >= 260) return false;
		unsigned visited{};
		auto* geometry = FindScreen(a_root, visited);
		if (!geometry) return false;
		ResourceKey key;
		REL::Relocation<MakeKey>{ REL::ID(137049) }(&key, std::string(a_materialPath).c_str());
		std::uint32_t material{};
		REL::Relocation<FindMaterial>{ REL::ID(129165) }(&material, &key);
		if (!material) {
			REX::ERROR("ScreenMaterial: missing {}", a_materialPath);
			return false;
		}
		// Same setter as ApplyMaterialVisitor (863c00). No optional parameter
		// override; its third register passes through unused in 247e5e0.
		REL::Relocation<SetMaterial>{ REL::ID(147289) }(geometry, material);
		const bool applied = Current(a_root) == material;
		REX::INFO("ScreenMaterial: applied {} id {} verified {}", a_materialPath, material, applied);
		return applied;
	}

	std::uint32_t Current(RE::NiAVObject* a_root)
	{
		unsigned visited{};
		auto* geometry = FindScreen(a_root, visited);
		if (!geometry || !Supported()) return 0;
		std::uintptr_t entry{};
		std::memcpy(&entry, static_cast<std::byte*>(geometry) + 0x170, sizeof(entry));
		// 247e5e0 keys the LOD material entry by the MaterialDB ID;
		// 247ec80 copies that key's third word to entry+10. Entry+20
		// is an instance handle, not an ID accepted by the setter.
		if (!entry) return 0;
		std::uint32_t id{};
		std::memcpy(&id, reinterpret_cast<const std::byte*>(entry) + 0x10, sizeof(id));
		return id;
	}

	bool ApplyID(RE::NiAVObject* a_root, std::uint32_t a_id)
	{
		unsigned visited{};
		auto* geometry = FindScreen(a_root, visited);
		if (!geometry || !a_id || !Supported()) return false;
		REL::Relocation<SetMaterial>{ REL::ID(147289) }(geometry, a_id);
		return Current(a_root) == a_id;
	}
}
