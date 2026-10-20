#include "World/ScreenBindings.h"
#include "World/ScreenMaterial.h"
#include "World/WorldAssets.h"
#include "Core/NativeMainThreadQueue.h"
#include "RE/N/NiNode.h"
#include "REX/FModule.h"
#include <unordered_set>

namespace OSFUI::ScreenBindings
{
    namespace
    {
        struct Binding
        {
            std::uint32_t token{}, original{}, applied{}, appliedToken{};
            // Diagnostic identity only. Never dereference these saved addresses.
            std::uintptr_t reference{}, root{};
            std::string view;
            bool privateGeometry{}, refresh{};
        };
        std::mutex mutex;
        std::unordered_map<std::uint32_t, Binding> bindings;
        std::unordered_set<std::string> views;
        std::uint32_t nextToken{1};
        bool enabled{};
        bool replacingSession{};
        std::unordered_set<std::uint32_t> bases;
        bool vanillaFixture{};

        bool Admitted(RE::TESObjectREFR* ref)
        {
            if (!ref || ref->IsDeleted() || !ref->data.objectReference) return false;
            const auto base=ref->data.objectReference->GetFormID();
            return (vanillaFixture && base == 0xC499F && ref->GetFormID() < 0xFF000000) ||
                (bases.contains(base) && ref->data.objectReference->Is(RE::FormType::kACTI));
        }

        bool Nodes(RE::NiAVObject* object, std::vector<std::uintptr_t>& nodes, unsigned depth = 0)
        {
            if (!object || depth > 16 || nodes.size() >= 128) return false;
            nodes.push_back(reinterpret_cast<std::uintptr_t>(object));
            if (auto* node=object->GetAsNiNode()) {
                if (node->childrenSize > node->childrenMaxSize || node->childrenSize > 128 || (node->childrenSize && !node->children)) return false;
                for (std::uint16_t i=0;i<node->childrenSize;++i)
                    if (node->children[i] && !Nodes(node->children[i],nodes,depth+1)) return false;
            }
            return true;
        }
    }

    void Enable(std::vector<std::string> a_views, std::vector<std::uint32_t> a_bases)
    {
        std::lock_guard lock(mutex);
        views = {a_views.begin(),a_views.end()};
        enabled = ScreenMaterial::Supported();
        bases = {a_bases.begin(), a_bases.end()};
        vanillaFixture = false;
    }

    void Enable(std::vector<std::string> a_views, std::uint32_t a_dynamicBase)
    {
        Enable(std::move(a_views), std::vector<std::uint32_t>{a_dynamicBase});
        std::lock_guard lock(mutex);
        vanillaFixture = true;
    }

    std::uint32_t Bind(std::uint32_t a_ref, std::string_view a_view)
    {
        std::lock_guard lock(mutex);
        if (!enabled || replacingSession || !a_ref || !views.contains(std::string(a_view)) || nextToken >= 0x7FFFFFFF ||
            !bindings.contains(a_ref) ||
            !bindings.at(a_ref).privateGeometry || !bindings.at(a_ref).original) return 0;
        auto& binding=bindings[a_ref];
        if (binding.token && binding.view == a_view) return binding.token;
        binding.token=nextToken++;
        binding.view=a_view;
        binding.refresh=true;
        REX::INFO("ScreenBindings: bound reference {} token {} view '{}'",a_ref,binding.token,binding.view);
        return binding.token;
    }

    bool Unbind(std::uint32_t a_token)
    {
        if (!a_token) return false;
        std::lock_guard lock(mutex);
        for (auto& [ref,binding] : bindings) {
            if (binding.token != a_token) continue;
            binding.token=0;
            binding.view.clear();
            binding.refresh=true;
            REX::INFO("ScreenBindings: unbound reference {} token {}",ref,a_token);
            return true;
        }
        return false;
    }

    void Reset()
    {
        std::lock_guard lock(mutex);
        for (auto it=bindings.begin();it!=bindings.end();) {
            if (!it->second.applied) { it=bindings.erase(it); continue; }
            auto& binding=it++->second;
            binding.token=0;
            binding.view.clear();
            binding.refresh=true;
        }
        // Never reuse tokens, including across save replacement.
    }

    void Invalidate(std::uint32_t a_ref)
    {
        std::lock_guard lock(mutex);
        if (const auto it=bindings.find(a_ref); it!=bindings.end()) {
            REX::INFO("ScreenBindings: invalidated reference {} token {}", a_ref, it->second.token);
            bindings.erase(it);
        }
    }

    void BeginSessionReplacement()
    {
        // The caller receives SaveLoadEvent BEGIN before world teardown.
        std::lock_guard lock(mutex);
        replacingSession=true;
        for (auto it=bindings.begin();it!=bindings.end();) {
            if (!it->second.applied) { it=bindings.erase(it); continue; }
            auto& binding=it++->second;
            binding.token=0;
            binding.view.clear();
            binding.refresh=true;
        }
        // Retain only scalar restoration metadata for a scene the loader reuses.
        // A new model-load callback discards it before capturing its original.
    }

    void EndSessionReplacement()
    {
        std::lock_guard lock(mutex);
        replacingSession=false;
        for (auto& [id,binding]:bindings) binding.refresh=true;
    }

    std::uint32_t Token(std::uint32_t a_ref)
    {
        std::lock_guard lock(mutex);
        const auto it=bindings.find(a_ref);
        return it==bindings.end() ? 0 : it->second.token;
    }

    bool HasBindings(std::string_view a_view)
    {
        std::lock_guard lock(mutex);
        return !replacingSession && std::ranges::any_of(bindings,[&](const auto& item) {
            return item.second.token && item.second.view==a_view;
        });
    }

    std::optional<Selection> Resolve(std::uint32_t a_token)
    {
        std::lock_guard lock(mutex);
        if (!a_token || replacingSession) return std::nullopt;
        for (const auto& [ref, binding] : bindings) {
            if (binding.token == a_token)
                return Selection{ref, binding.view,
                    binding.privateGeometry && binding.applied && binding.appliedToken == a_token && !binding.refresh};
        }
        return std::nullopt;
    }

    void OnSetup(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root, RE::NiAVObject* a_cachedSource)
    {
        std::lock_guard lock(mutex);
        if (!enabled || !Admitted(a_ref) || !a_root) return;
        const auto id=a_ref->GetFormID();
        if (!bindings.contains(id) && bindings.size() >= 4096) return;
        auto& binding=bindings[id];
        const auto reference=reinterpret_cast<std::uintptr_t>(a_ref);
        const auto root=reinterpret_cast<std::uintptr_t>(a_root);
        // Logical identity survives reconstruction; scene evidence does not.
        // The load callback is also a boundary when the allocator reuses addresses.
        if (a_cachedSource || reference!=binding.reference || root!=binding.root) {
            binding.original=0;
            binding.applied=0;
            binding.privateGeometry=false;
        }
        binding.reference=reference;
        const auto current=ScreenMaterial::Current(a_root);
        if (a_cachedSource) {
            std::vector<std::uintptr_t> source, target;
            binding.privateGeometry=Nodes(a_cachedSource,source) && Nodes(a_root,target) &&
                std::ranges::none_of(target,[&](auto address){return std::ranges::find(source,address)!=source.end();});
        } else if (root != binding.root) {
            binding.privateGeometry=false;
        }
        // The loader exposes the private graph before material loading finishes.
        // Keep that graph proof so its following appearance refresh can capture
        // the original ID, but never bind while the ID is unavailable.
        if (!current) { binding.root=root; return; }
        if (!binding.original || (current != binding.applied && current != binding.original)) {
            binding.original=current;
            binding.applied=0;
        }
        binding.root=root;
        if (!binding.privateGeometry) return;
        if (!binding.view.empty()) {
            if (ScreenMaterial::Apply(a_root,WorldAssets::ScreenMaterialPath(binding.view,ScreenMaterial::FullUV(a_root)))) {
                binding.applied=ScreenMaterial::Current(a_root);
                binding.appliedToken=binding.token;
            }
        } else if (binding.applied && current == binding.applied) {
            if (ScreenMaterial::ApplyID(a_root,binding.original)) binding.applied=0;
        }
    }

    void Tick()
    {
        if (!NativeMainThreadQueue::SnapshotState().insideDrain) return;
        auto* ui=RE::UI::GetSingleton();
        if (!ui) return;
        static bool wasMainMenu{};
        const bool mainMenu=ui->IsMenuOpen(RE::BSFixedString("MainMenu"));
        if (mainMenu && !wasMainMenu) Reset();
        wasMainMenu=mainMenu;
        if (mainMenu || ui->IsMenuOpen(RE::BSFixedString("LoadingMenu"))) return;
        std::vector<std::uint32_t> refs;
        {
            std::lock_guard lock(mutex);
            if (replacingSession) return;
            for (auto& [ref,binding]:bindings) if (binding.refresh) refs.push_back(ref);
        }
        using Enqueue=void (*)(RE::TESObjectREFR**);
        REL::Relocation<Enqueue> enqueue{REL::ID(38129)};
        if (enqueue.address()!=REX::FModule::GetExecutingModule().GetBaseAddress()+0x35ABA0) return;
        for (const auto id:refs) {
            auto* ref=RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            // Absence during cell unload is not deletion. Keep the selection.
            if (!ref) {
                std::lock_guard lock(mutex);
                const auto it=bindings.find(id);
                // An absent scene has nothing left to restore after reset/unbind.
                if (it!=bindings.end() && !it->second.token) bindings.erase(it);
                continue;
            }
            {
                std::lock_guard lock(mutex);
                const auto it=bindings.find(id);
                if (replacingSession || it==bindings.end()) continue;
                if (!Admitted(ref)) { bindings.erase(it); continue; }
                it->second.refresh=false;
            }
            enqueue(&ref);
        }
    }
}
