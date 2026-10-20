#include "World/EngineTextureIdentity.h"
#include "Composite/D3D12Prologue.h"
#include "Core/Log.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"
#include <cstring>

namespace OSFUI::EngineTextureIdentity
{
	namespace
	{
		thread_local std::optional<WorldAssets::Key> current;
		using CreateFn = int (*)(const std::byte*);
		CreateFn original = nullptr;
		int      Create(const std::byte* queued)
		{
			const std::byte* entry = nullptr;
			std::memcpy(&entry, queued + 0xe0, sizeof(entry));
			const auto saved = current;
			current.reset();
			if (entry) {
				WorldAssets::Key key;
				std::memcpy(&key, entry + 8, sizeof(key));
				current = key;
			}
			struct Restore
			{
				std::optional<WorldAssets::Key> value;
				~Restore() { current = value; }
			} restore{ saved };
			return original(queued);
		}
	}

	std::optional<WorldAssets::Key> CurrentAsset() { return current; }

	bool Install()
	{
		// .244: TextureDB queued creation -> CreateTexture. The scoped call is
		// synchronous through AAL::Texture creation and all its initial SRVs.
		// Streaming/unknown creation routes are deliberately not intercepted.
		const auto   site = REL::Relocation<std::uintptr_t>{ REL::ID(139982) }.address() + 0x76;
		const auto   target = REL::Relocation<std::uintptr_t>{ REL::ID(140026) }.address();
		const auto   base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		std::int32_t displacement = 0;
		std::memcpy(&displacement, reinterpret_cast<const void*>(site + 1), sizeof(displacement));
		if (site != base + 0x28f8826 || target != base + 0x28fd000 ||
			*reinterpret_cast<const std::uint8_t*>(site) != 0xe8 || site + 5 + displacement != target) {
			REX::ERROR("WorldTexture: unsupported texture identity call site; world binding disabled (verified runtime: 1.16.244.0)");
			return false;
		}
		original = reinterpret_cast<CreateFn>(target);
		REL::GetTrampoline().write_call<5>(site, &Create);
		REX::INFO("WorldTexture: verified TextureDB asset identity integration installed");
		return true;
	}
}