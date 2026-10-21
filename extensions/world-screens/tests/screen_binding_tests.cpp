#include "stubs/Engine.h"
// Compile the actual private registry with engine-call doubles.
#include "../src/World/ScreenBindings.cpp"
#include "World/ScreenInteraction.h"
#include <cstdio>
#include <cstdlib>

namespace OSFUI::NativeMainThreadQueue {
    QueueState SnapshotState() { QueueState state; state.insideDrain=BindingTest::insideDrain; return state; }
}
namespace OSFUI::WorldAssets {
    std::string ScreenMaterialPath(std::string_view view, bool) { return std::string(view); }
}
namespace OSFUI::ScreenMaterial {
    bool Supported() { return true; }
    bool FullUV(RE::NiAVObject* root) { return root->fullUV; }
    std::uint32_t Current(RE::NiAVObject* root) { return root->material; }
    bool ApplyID(RE::NiAVObject* root, std::uint32_t id) { ++BindingTest::applied; root->material=id; return true; }
    bool Apply(RE::NiAVObject* root, std::string_view path) { return ApplyID(root,path=="mod/a" ? 28 : 31); }
}
void BindingTest::Enqueue(RE::TESObjectREFR** ref) {
    ++enqueued;
    if ((*ref)->scene) OSFUI::ScreenBindings::OnSetup(*ref,(*ref)->scene,nullptr);
}

unsigned checks{};
void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::fprintf(stderr,"FAILED: %s\n",name); std::exit(1); }
}
int main() {
    namespace S=OSFUI::ScreenBindings;
    RE::TESForm base; base.id=0x6000800;
    RE::NiNode source, aRoot, bRoot, rebuilt, reused, unknown;
    RE::TESObjectREFR a,b,aReplacement,aReuse;
    a.id=0xFF001001; a.data.objectReference=&base; a.scene=&aRoot;
    b.id=0xFF001002; b.data.objectReference=&base; b.scene=&bRoot;
    RE::TESForm::forms={{a.id,&a},{b.id,&b}};
    S::Enable({"mod/a","mod/b"},base.id);
    Check(!S::Bind(a.id,"mod/a"),"unobserved references are refused");
    S::OnSetup(&a,&aRoot,&aRoot);
    Check(!S::Bind(a.id,"mod/a"),"shared cached graph is refused");
    aRoot.material=0;
    S::OnSetup(&a,&aRoot,&source);
    Check(!S::Bind(a.id,"mod/a"),"graph readiness alone cannot establish the original material");
    aRoot.material=491362;
    S::OnSetup(&a,&aRoot,nullptr);
    S::OnSetup(&b,&bRoot,&source);
    Check(!S::Bind(a.id,"mod/missing"),"unknown views are refused");
    const auto aToken=S::Bind(a.id,"mod/a"), bToken=S::Bind(b.id,"mod/a");
    Check(aToken && bToken && aToken!=bToken,"mirrors have distinct binding tokens");
    Check(S::Bind(a.id,"mod/a")==aToken,"bind of the same selection is idempotent");
    Check(S::Resolve(aToken) && !S::Resolve(aToken)->ready,"input waits for the selected material");
    S::Tick();
    Check(S::Resolve(aToken)->ready,"applied binding can admit input");
    Check(aRoot.material==28 && bRoot.material==28,"both mirrors apply their output");
    Check(S::CountBindings()==std::unordered_map<std::string,std::uint32_t>{{"mod/a",2}},
        "binding count includes each mirror independently of their shared feed");
    const auto appliedBefore=BindingTest::applied;
    S::OnSetup(&a,&aRoot,nullptr);
    Check(BindingTest::applied==appliedBefore,"unchanged appearance callback skips redundant material lookup and setter");
    aRoot.fullUV=true;
    S::OnSetup(&a,&aRoot,nullptr);
    Check(BindingTest::applied==appliedBefore+1,"changed UV variant reapplies its selected material");
    aRoot.fullUV=false;
    S::OnSetup(&a,&aRoot,nullptr);
    Check(BindingTest::applied==appliedBefore+2,"restoring UV variant reapplies its selected material");
    namespace I=OSFUI::ScreenInteraction;
    Check(I::Decide(false,false,S::Resolve(aToken),"mod/a",false)==I::Action::Forward,
        "ready selected binding forwards interaction to core");
    Check(I::Decide(true,false,S::Resolve(aToken),"mod/a",true)==I::Action::Keep,
        "forwarded interaction remains valid without an admission timeout");
    Check(S::ReadyReferences("mod/a").size()==2,"ready mirror snapshot supplies both references");
    a.scene=&unknown;
    S::OnSetup(&a,&unknown,nullptr);
    Check(S::Resolve(aToken) && !S::Resolve(aToken)->ready && S::Resolve(bToken)->ready,
        "selected graph invalidation preserves logical binding and the other ready mirror");
    Check(I::Decide(true,false,S::Resolve(aToken),"mod/a",false)==I::Action::End,
        "selected graph invalidation ends a forwarded or active interaction despite a healthy mirror");
    Check(I::Decide(false,false,S::Resolve(aToken),"mod/a",false)==I::Action::Wait,
        "unforwarded interaction can wait for selected graph recovery");
    Check(S::ReadyReferences("mod/a").size()==1,"invalidated geometry is excluded from audio candidates");
    Check(S::CountBindings().at("mod/a")==2,"logical screen count includes an unready reference excluded from audio");
    a.scene=&aRoot;
    aRoot.material=491362;
    S::OnSetup(&a,&aRoot,&source);
    Check(I::Decide(false,false,S::Resolve(aToken),"mod/a",false)==I::Action::Forward,
        "restored selected geometry can admit a new interaction");
    aRoot.material=0;
    S::OnSetup(&a,&aRoot,nullptr);
    Check(I::Decide(true,false,S::Resolve(aToken),"mod/a",false)==I::Action::End,
        "temporarily missing material also ends the forwarded interaction");
    aRoot.material=491362;
    S::OnSetup(&a,&aRoot,nullptr);
    Check(S::Resolve(aToken)->ready,"same-graph material recovery restores readiness");
    Check(I::Decide(true,true,S::Resolve(aToken),"mod/a",false)==I::Action::End,
        "explicit cancellation still ends a ready forwarded interaction");
    Check(I::Decide(true,false,S::Resolve(aToken),"mod/b",false)==I::Action::End,
        "view replacement cannot retain a forwarded interaction");
    Check(I::Decide(false,false,S::Resolve(aToken),"mod/a",true)==I::Action::Reject,
        "pending admission timeout is still enforced");
    Check(S::Unbind(aToken) && !S::Unbind(aToken),"unbind invalidates its token once");
    S::Tick();
    Check(aRoot.material==491362 && bRoot.material==28,"unbind restores only its own monitor");
    Check(S::CountBindings().at("mod/a")==1,"unbind removes only its own logical screen count");
    const auto nextToken=S::Bind(a.id,"mod/b");
    Check(nextToken>aToken,"tokens are not reused");
    Check(!S::Resolve(aToken) && !S::Resolve(nextToken)->ready,"replacement invalidates input and waits for its own material");
    // Reference temporarily absent while a pending refresh exists.
    RE::TESForm::forms.erase(a.id);
    const auto queued=BindingTest::enqueued;
    S::Tick();
    Check(S::Token(a.id)==nextToken && BindingTest::enqueued==queued,"unloaded reference retains selection without enqueue");
    Check(S::CountBindings()==std::unordered_map<std::string,std::uint32_t>{{"mod/a",1},{"mod/b",1}},
        "logical counts retain unloaded references on their selected view");
    aReplacement.id=a.id; aReplacement.data.objectReference=&base; aReplacement.scene=&rebuilt;
    RE::TESForm::forms[a.id]=&aReplacement;
    rebuilt.material=0;
    S::OnSetup(&aReplacement,&rebuilt,&source);
    Check(S::Token(a.id)==nextToken && rebuilt.material==0,"reconstruction retains token but waits for material");
    rebuilt.material=491362;
    S::OnSetup(&aReplacement,&rebuilt,nullptr);
    Check(rebuilt.material==31,"new reference object reapplies its selected view");
    aReplacement.scene=&unknown;
    S::OnSetup(&aReplacement,&unknown,nullptr);
    Check(unknown.material==491362,"an unproven new graph stays unchanged");
    S::OnSetup(&aReplacement,&unknown,&source);
    Check(unknown.material==31,"new graph can bind after isolation proof");
    // Logical deletion precedes an unrelated object's reuse of the same ID.
    S::Invalidate(a.id);
    Check(!S::Token(a.id) && !S::Unbind(nextToken),"deletion invalidates stale token");
    aReuse.id=a.id; aReuse.data.objectReference=&base; aReuse.scene=&reused;
    RE::TESForm::forms[a.id]=&aReuse;
    S::OnSetup(&aReuse,&reused,&source);
    Check(!S::Token(a.id) && reused.material==491362,"reused ID inherits no selection");
    Check(S::Token(b.id)==bToken && bRoot.material==28,"deleting one reference preserves another mirror");
    const auto reuseToken=S::Bind(a.id,"mod/b");
    Check(reuseToken>nextToken,"reused ID obtains a fresh token");
    S::Tick();
    S::BeginSessionReplacement();
    Check(!S::Token(a.id) && !S::Token(b.id),"load begin clears old token authority");
    Check(!S::Unbind(reuseToken) && !S::Unbind(bToken),"old tokens are rejected during load");
    Check(!S::Bind(a.id,"mod/a"),"binding is suspended during load");
    Check(S::CountBindings().empty(),"session replacement clears logical screen counts");
    const auto beforeLoad=BindingTest::enqueued;
    S::Tick();
    Check(BindingTest::enqueued==beforeLoad,"no explicit appearance work queued during load");
    // Same-cell save load may keep the existing scene addresses and material.
    S::OnSetup(&aReuse,&reused,nullptr);
    Check(reused.material==491362,"same-scene replacement restores original appearance");
    S::EndSessionReplacement(); S::Tick();
    Check(bRoot.material==491362,"completion restores a retained scene with no loader callback");
    const auto fresh=S::Bind(a.id,"mod/a");
    Check(fresh>reuseToken,"new session cannot reuse a stale token");
    BindingTest::insideDrain=false; const auto before=BindingTest::enqueued;
    S::Tick(); Check(BindingTest::enqueued==before,"wrong execution context cannot enqueue");
    BindingTest::insideDrain=true; S::Tick();
    S::Reset(); S::Tick();
    Check(!S::Unbind(fresh) && reused.material==491362,"reset retires tokens and restores appearance");
    const auto first=S::Bind(a.id,"mod/a"); S::Tick();
    const auto changed=S::Bind(a.id,"mod/b"); S::Tick();
    Check(changed>first && !S::Unbind(first) && reused.material==31,"selection replacement retires old handle and applies new view");
    Check(S::CountBindings()==std::unordered_map<std::string,std::uint32_t>{{"mod/b",1}},"browser demand follows the selected company");
    S::Unbind(changed); S::Tick();
    Check(S::CountBindings().empty() && reused.material==491362,"last unbind releases browser demand and restores original");
    const auto missingMaterial=S::Bind(a.id,"mod/a"); S::Tick();
    reused.material=0;
    S::OnSetup(&aReuse,&reused,nullptr);
    Check(!S::Resolve(missingMaterial)->ready,"missing material revokes readiness without losing restoration metadata");
    S::Unbind(missingMaterial);
    reused.material=28;
    S::Tick();
    Check(reused.material==491362,"unbind restores the original after same-graph material recovery");
    RE::TESForm otherBase; otherBase.id=0x07000800;
    RE::TESObjectREFR placed; RE::NiNode placedRoot;
    placed.id=0x07000900; placed.data.objectReference=&otherBase; placed.scene=&placedRoot;
    RE::TESForm::forms[placed.id]=&placed;
    S::Enable({"mod/a","mod/b"},std::vector<std::uint32_t>{base.id,otherBase.id});
    S::OnSetup(&placed,&placedRoot,&source);
    const auto placedToken=S::Bind(placed.id,"mod/a");
    Check(placedToken!=0,"registered static placements work alongside workshop references");
    S::Tick();
    Check(placedRoot.material==28 && S::Resolve(placedToken)->reference==placed.id,"placed screen owns its binding");
    S::Invalidate(placed.id);
    Check(!S::Resolve(placedToken),"deletion revokes the screen interaction capability");
    std::printf("%u screen binding checks passed (engine doubles; not runtime proof)\n",checks);
}
