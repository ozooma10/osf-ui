#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// State-machine doubles only: these do not model engine threading, ownership,
// ABI, event delivery, or material resources. Runtime acceptance is separate.
namespace RE {
    enum class FormType { kACTI, kSTAT };
    class TESForm {
    public:
        std::uint32_t id{};
        FormType type{FormType::kACTI};
        std::uint32_t GetFormID() const { return id; }
        bool Is(FormType value) const { return type==value; }
        inline static std::unordered_map<std::uint32_t,TESForm*> forms;
        template<class T> static T* LookupByID(std::uint32_t id) {
            const auto it=forms.find(id);
            return it==forms.end() ? nullptr : static_cast<T*>(it->second);
        }
    };
    class NiNode;
    class NiAVObject {
    public:
        std::uint32_t material{491362};
        virtual NiNode* GetAsNiNode() { return nullptr; }
    };
    class NiNode : public NiAVObject {
    public:
        std::uint16_t childrenSize{}, childrenMaxSize{};
        NiAVObject** children{};
        NiNode* GetAsNiNode() override { return this; }
    };
    class TESObjectREFR : public TESForm {
    public:
        struct { TESForm* objectReference{}; } data;
        bool deleted{};
        NiAVObject* scene{};
        bool IsDeleted() const { return deleted; }
    };
    using BSFixedString=std::string_view;
    class UI {
    public:
        bool loading{}, mainMenu{};
        static UI* GetSingleton() { static UI instance; return &instance; }
        bool IsMenuOpen(BSFixedString name) const {
            return name=="MainMenu" ? mainMenu : name=="LoadingMenu" && loading;
        }
    };
}
namespace BindingTest {
    inline bool insideDrain{true};
    inline unsigned enqueued{}, applied{};
    void Enqueue(RE::TESObjectREFR**);
}
namespace REL {
    struct ID { unsigned value; };
    template<class T> class Relocation {
    public:
        explicit Relocation(ID) {}
        std::uintptr_t address() const { return 0x35ABA0; }
        void operator()(RE::TESObjectREFR** ref) const { BindingTest::Enqueue(ref); }
    };
}
namespace REX {
    struct FModule {
        static FModule GetExecutingModule() { return {}; }
        std::uintptr_t GetBaseAddress() const { return 0; }
    };
    template<class... Args> void INFO(const char*, Args&&...) {}
}
