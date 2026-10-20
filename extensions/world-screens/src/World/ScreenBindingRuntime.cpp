#include "World/ScreenBindingRuntime.h"
#include "World/ScreenBindings.h"
#include "World/ScreenMaterial.h"
#include "Extension.h"
#include "RE/E/Events.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESObjectACTI.h"
#include "Core/StringUtil.h"
#include "Core/Json.h"
#include <charconv>
#include "REL/Trampoline.h"
#include "REX/FModule.h"

namespace OSFUI::ScreenBindingRuntime
{
    namespace
    {
        bool installed{};
        std::atomic_bool enabled{};
        std::vector<std::string> viewIds;
        using Setup = void (*)(RE::TESForm*, RE::TESObjectREFR*, RE::NiPointer<RE::NiAVObject>*, std::uintptr_t);
        using LoadModel = void (*)(RE::TESForm*, RE::TESObjectREFR*, RE::NiPointer<RE::NiAVObject>*, RE::NiPointer<RE::NiAVObject>*, bool, float, void*);
        Setup setup{};
        LoadModel load{};
        thread_local RE::NiAVObject* cached{};
        template<class T> T Read(std::uintptr_t address) { T value; std::memcpy(&value,reinterpret_cast<void*>(address),sizeof(value)); return value; }

        std::vector<std::uint32_t> RegisteredBases()
        {
            std::vector<std::uint32_t> result;
            auto* handler = RE::TESDataHandler::GetSingleton();
            const auto folder = Extension::DataDir() / "screens";
            if (!handler || !std::filesystem::is_directory(folder)) return result;
            for (const auto& file : std::filesystem::directory_iterator(folder)) {
                if (!file.is_regular_file() || file.path().extension() != ".json" || file.file_size() > 65536) continue;
                try {
                    const auto document = Json::ParseFile(file.path());
                    if (!document || document->at("version") != 1 || !document->at("bases").is_array() ||
                        document->at("bases").size() > 256) throw std::runtime_error("invalid screen catalogue");
                    for (const auto& entry : document->at("bases")) {
                        const auto plugin = entry.at("plugin").get<std::string>();
                        const auto localText = entry.at("formId").get<std::string>();
                        const auto profile = entry.at("profile").get<std::string>();
                        std::uint32_t local{};
                        const auto parsed = std::from_chars(localText.data(), localText.data() + localText.size(), local, 16);
                        if ((profile != "display-screen5" && profile != "full-uv") || plugin.empty() || plugin.find_first_of("/\\:") != plugin.npos ||
                            parsed.ec != std::errc{} || parsed.ptr != localText.data() + localText.size() || !local || local > 0xFFFFFF)
                            throw std::runtime_error("invalid screen base or unsupported profile");
                        RE::TESForm* base{};
                        for (auto* loaded : handler->compiledFileCollection.files) {
                            if (loaded && StringUtil::EqualsCaseInsensitiveAscii(loaded->fileName, plugin) && loaded->compileIndex < 0xFD) {
                                base = RE::TESForm::LookupByID((static_cast<std::uint32_t>(loaded->compileIndex) << 24) | local);
                                break;
                            }
                        }
                        // An optional content plugin may not be enabled in this profile.
                        if (!base) continue;
                        if (!base->Is(RE::FormType::kACTI)) throw std::runtime_error("screen base is not an activator");
                        auto model = StringUtil::ToLowerAscii(static_cast<RE::TESObjectACTI*>(base)->model.c_str());
                        std::ranges::replace(model, '\\', '/');
                        const auto expected = profile == "display-screen5" ? "setdressing/displayscreens/displayscreen5.nif" :
                            StringUtil::ToLowerAscii(entry.at("model").get<std::string>());
                        if (model != expected || (profile == "full-uv" && (!model.starts_with("osfui/screens/") ||
                            model.find("..") != model.npos || !model.ends_with(".nif"))))
                            throw std::runtime_error("screen base does not use the supported model");
                        if (std::ranges::find(result, base->GetFormID()) == result.end() && result.size() < 512)
                            result.push_back(base->GetFormID());
                    }
                } catch (const std::exception& e) {
                    REX::ERROR("ScreenBindingRuntime: {}: {}", file.path().filename().string(), e.what());
                }
            }
            return result;
        }

        void SetupHook(RE::TESForm* base, RE::TESObjectREFR* ref, RE::NiPointer<RE::NiAVObject>* root, std::uintptr_t context)
        {
            setup(base,ref,root,context);
            if (root && *root) {
                try { ScreenBindings::OnSetup(ref,root->get(),cached); }
                catch (const std::exception& e) { REX::ERROR("ScreenBindingRuntime: setup failed: {}",e.what()); }
            }
        }
        void LoadHook(RE::TESForm* base, RE::TESObjectREFR* ref, RE::NiPointer<RE::NiAVObject>* source,
            RE::NiPointer<RE::NiAVObject>* root, bool flag, float scale, void* context)
        {
            struct Restore { RE::NiAVObject* saved; ~Restore() { cached=saved; } } restore{cached};
            cached=source ? source->get() : nullptr;
            load(base,ref,source,root,flag,scale,context);
        }
        class SessionSink final : public RE::BSTEventSink<RE::TESFormDeleteEvent>,
            public RE::BSTEventSink<RE::TESFormIDRemapEvent>, public RE::BSTEventSink<RE::SaveLoadEvent>,
            public RE::BSTEventSink<RE::TESLoadGameEvent>
        {
            bool replacing{};
            RE::BSEventNotifyControl ProcessEvent(const RE::TESFormDeleteEvent& e, RE::BSTEventSource<RE::TESFormDeleteEvent>*) override
            { ScreenBindings::Invalidate(e.formID); return RE::BSEventNotifyControl::kContinue; }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESFormIDRemapEvent& e, RE::BSTEventSource<RE::TESFormIDRemapEvent>*) override
            { ScreenBindings::Invalidate(e.oldFormID); ScreenBindings::Invalidate(e.newFormID); return RE::BSEventNotifyControl::kContinue; }
            RE::BSEventNotifyControl ProcessEvent(const RE::SaveLoadEvent& e, RE::BSTEventSource<RE::SaveLoadEvent>*) override
            {
                const auto op=static_cast<unsigned>(e.opType);
                if (op==2 || op==4 || op==6 || op==11) {
                    if (static_cast<unsigned>(e.status)==0) { replacing=true; ScreenBindings::BeginSessionReplacement(); }
                    else if (replacing) { replacing=false; ScreenBindings::EndSessionReplacement(); }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent&, RE::BSTEventSource<RE::TESLoadGameEvent>*) override
            {
                if (!replacing) { ScreenBindings::BeginSessionReplacement(); ScreenBindings::EndSessionReplacement(); }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }
    bool Enabled() { return enabled; }
    void Configure(std::vector<std::string> views) { viewIds=std::move(views); }
    void Install()
    {
        // Never install two owners of the same hook during the original fixture.
        const auto module=REX::FModule::GetExecutingModule();
        if (!ScreenMaterial::Supported()) return;
        const auto image=module.GetBaseAddress();
        REL::Relocation<std::uintptr_t> table{REL::ID(411455)};
        const auto expected=REL::Relocation<std::uintptr_t>{REL::ID(61019)}.address();
        const auto site=REL::Relocation<std::uintptr_t>{REL::ID(61026)}.address()+0xBE;
        const auto target=REL::Relocation<std::uintptr_t>{REL::ID(61018)}.address();
        if (table.address()!=image+0x4B839C0 || expected!=image+0xA6F500 ||
            Read<std::uintptr_t>(table.address()+0x408)!=expected || site!=image+0xA70E6E ||
            target!=image+0xA6E500 || Read<std::uint8_t>(site)!=0xE8 || site+5+Read<std::int32_t>(site+1)!=target) {
            REX::ERROR("ScreenBindingRuntime: appearance hook gate failed"); return;
        }
        setup=reinterpret_cast<Setup>(expected);
        load=reinterpret_cast<LoadModel>(target);
        REL::GetTrampoline().write_call<5>(site,&LoadHook);
        table.write_vfunc(0x81,&SetupHook);
        installed=true;
    }
    void OnDataLoaded()
    {
        if (!installed) return;
        const auto image=REX::FModule::GetExecutingModule().GetBaseAddress();
        if (REL::Relocation<std::uintptr_t>{REL::ID(64137)}.address()!=image+0xB99D20 ||
            REL::Relocation<std::uintptr_t>{REL::ID(64138)}.address()!=image+0xB99D90 ||
            REL::Relocation<std::uintptr_t>{REL::ID(82710)}.address()!=image+0x12EDC50) return;
        auto* deleted=RE::TESFormDeleteEvent::GetEventSource();
        auto* remapped=RE::TESFormIDRemapEvent::GetEventSource();
        auto* saved=RE::SaveLoadEvent::GetEventSource();
        auto* loaded=RE::TESLoadGameEvent::GetEventSource();
        if (reinterpret_cast<std::uintptr_t>(deleted)!=image+0x5977EA0 ||
            reinterpret_cast<std::uintptr_t>(remapped)!=image+0x5977EC8 ||
            reinterpret_cast<std::uintptr_t>(saved)!=image+0x61EEA40 || !loaded ||
            Read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(deleted))!=image+0x4B981B0 ||
            Read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(remapped))!=image+0x4B98180 ||
            Read<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(saved))!=image+0x4C448D0) return;
        auto bases = RegisteredBases();
        if (bases.empty()) return;
        static SessionSink sink;
        deleted->RegisterSink(&sink); remapped->RegisterSink(&sink); saved->RegisterSink(&sink); loaded->RegisterSink(&sink);
        const auto count = bases.size();
        ScreenBindings::Enable(viewIds,std::move(bases));
        enabled=true;
        REX::INFO("ScreenBindingRuntime: enabled {} supported screen bases and {} named views",count,viewIds.size());
    }
}
