#include "World/ScreenAudio.h"
#include "World/ScreenBindings.h"
#include "Core/NativeMainThreadQueue.h"
#include "RE/N/NiCamera.h"
#include "RE/S/SceneGraph.h"
#include "RE/I/INIPrefSettingCollection.h"
#include "BrowserAudio.h"
#include "REX/FModule.h"

namespace OSFUI::ScreenAudio
{
    API::Surfaces::AudioScene Capture()
    {
        using namespace osfui::browser::audio;
        AudioScene scene;
        if (!NativeMainThreadQueue::SnapshotState().insideDrain) return scene;
        auto* ui=RE::UI::GetSingleton();
        if (!ui || ui->IsMenuOpen(RE::BSFixedString("MainMenu")) ||
            ui->IsMenuOpen(RE::BSFixedString("LoadingMenu"))) return scene;
        const auto references=ScreenBindings::ReadyReferences("osfui.browser/tv");
        if (references.empty()) return scene;
        // Main::WorldRoot's CommonLib ID is still zero. This exact-version and
        // instruction gate resolves the main scene camera, NOT PlayerCamera's
        // viewmodel camera. Evidence: camera.gpu_view + 1.16.244 lens writer.
        const auto module=REX::FModule::GetExecutingModule();
        const auto image=module.GetBaseAddress();
        static const bool cameraSupported=[&] {
            const unsigned char expected[]{0x48,0x8B,0x05,0xCD,0x4A,0x12,0x04,0x48,0x8B,0x88,0x80,0,0,0};
            return module.GetFileVersion()==REL::Version(1,16,244,0) &&
                std::memcmp(reinterpret_cast<const void*>(image+0x1E8BE14),expected,sizeof(expected))==0;
        }();
        if (!cameraSupported) return scene;
        auto* graph=*reinterpret_cast<RE::SceneGraph**>(image+0x5FB08E8);
        const auto camera=graph ? graph->camera : RE::NiPointer<RE::NiCamera>{};
        if (!camera) return scene;
        const auto& world=camera->world;
        scene.position={world.translate.x,world.translate.y,world.translate.z};
        scene.front={world.rotate[0].x,world.rotate[0].y,world.rotate[0].z};
        scene.up={world.rotate[1].x,world.rotate[1].y,world.rotate[1].z};
        if (auto* prefs=RE::INIPrefSettingCollection::GetSingleton())
            scene.gain=std::clamp(prefs->GetSetting<float>("fAudioVolumeMaster:Audio",1.f),0.f,1.f);
        if (!Valid(scene)) return {};
        std::vector<AudioEmitter> candidates;
        for (const auto& binding:references) {
            auto* ref=RE::TESForm::LookupByID<RE::TESObjectREFR>(binding.reference);
            if (!ref || reinterpret_cast<std::uintptr_t>(ref)!=binding.object || ref->IsDeleted() ||
                !ref->data.objectReference || ref->data.objectReference->GetFormID()!=binding.base ||
                ref->IsDisabled() || !ref->parentCell || !ref->parentCell->IsAttached()) continue;
            const auto loaded=ref->loadedData.LockRead();
            if (!*loaded || !(*loaded)->data3D ||
                reinterpret_cast<std::uintptr_t>((*loaded)->data3D.get())!=binding.root) continue;
            const auto& position=(*loaded)->data3D->world.translate;
            AudioVector point{position.x,position.y,position.z};
            // Both transforms are in render space, so floating-origin shifts
            // cancel; do not mix TESObjectREFR::data.location into this snapshot.
            if (Finite(point) && DistanceSquared(point,scene.position)<=kRange*kRange)
                candidates.push_back({binding.token,point});
        }
        std::ranges::sort(candidates,[&](const auto& a,const auto& b) {
            const auto da=DistanceSquared(a.position,scene.position), db=DistanceSquared(b.position,scene.position);
            return da==db ? a.id<b.id : da<db;
        });
        scene.count=static_cast<unsigned>(std::min<std::size_t>(candidates.size(),kMaxAudioEmitters));
        std::copy_n(candidates.begin(),scene.count,scene.emitters);
        return scene;
    }
}
