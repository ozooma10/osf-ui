#include "BrowserAudio.h"
#include "BrowserMessages.h"
#include "check.h"
#include <limits>
#include <iostream>

int main()
{
    using namespace osfui::browser::audio;
    AudioScene scene;
    CHECK(Valid(scene));
    scene.position={10,20,30};
    scene.count=2; scene.emitters[0]={11,{10,22,30}}; scene.emitters[1]={12,{13,20,30}};
    const auto encoded=Encode(scene);
    const auto decoded=Decode(encoded);
    CHECK(decoded && decoded->count==2 && decoded->emitters[1].id==12);
    CHECK(DistanceSquared(scene.position,scene.emitters[0].position)==4);
    CHECK(DistanceSquared(scene.position,scene.emitters[1].position)==9);
    // Render-origin shifts must leave relative distances unchanged.
    CHECK(DistanceSquared({1010,-980,30},{1010,-978,30})==4);
    for (const auto value : {nlohmann::json(),nlohmann::json::array(),nlohmann::json::object()}) CHECK(!Decode(value));
    for (const auto key:{"position","front","up","gain","emitters"}) {
        auto bad=encoded; bad.erase(key); CHECK(!Decode(bad));
    }
    for (const auto key:{"position","front","up"}) {
        auto bad=encoded; bad[key]={1,2}; CHECK(!Decode(bad));
        bad[key]={1,2,3,4}; CHECK(!Decode(bad));
        bad[key]={"x",2,3}; CHECK(!Decode(bad));
    }
    for (float gain:{-1.f,1.01f,std::numeric_limits<float>::infinity()}) {
        auto bad=scene; bad.gain=gain; CHECK(!Valid(bad));
    }
    scene.gain=0; CHECK(Valid(scene)); scene.gain=1;
    auto bad=scene; bad.up=bad.front; CHECK(!Valid(bad));
    bad=scene; bad.front={0,0,0}; CHECK(!Valid(bad));
    bad=scene; bad.emitters[1].id=11; CHECK(!Valid(bad));
    bad=scene; bad.emitters[1].id=0; CHECK(!Valid(bad));
    bad=scene; bad.position.x=std::numeric_limits<float>::quiet_NaN(); CHECK(!Valid(bad));
    bad=scene; bad.emitters[1].position.z=std::numeric_limits<float>::infinity(); CHECK(!Valid(bad));
    bad=scene; bad.count=kMaxAudioEmitters+1; CHECK(!Valid(bad));
    for (const auto id:{nlohmann::json(-1),nlohmann::json(1.5),nlohmann::json(4294967296ull)}) {
        auto value=encoded; value["emitters"][0]["id"]=id; CHECK(!Decode(value));
    }
    auto tooMany=encoded;
    for (unsigned i=2;i<=kMaxAudioEmitters;++i) tooMany["emitters"].push_back({{"id",100+i},{"position",{0,0,0}}});
    CHECK(!Decode(tooMany));
    float single[]{.6f,.8f}; Normalize(single,2); CHECK(single[0]==.6f && single[1]==.8f);
    float cluster[]{6,8}; Normalize(cluster,2); CHECK(std::abs(cluster[0]-.6f)<1e-6 && std::abs(cluster[1]-.8f)<1e-6);
    float silent[]{0,0}; Normalize(silent,2); CHECK(silent[0]==0 && silent[1]==0);
    CHECK(Fresh(1500,1000)); CHECK(!Fresh(1000,1500)); CHECK(!Fresh(1000,1000));
    CHECK(!Fresh(UINT64_MAX,1000)); CHECK(!Fresh(2001,1000)); CHECK(!Fresh(0,0));
    const osfui::browser::msg::BrowserAudio wire{.scene=encoded,.expiresMs=321};
    const auto copy=osfui::browser::msg::FromJson<osfui::browser::msg::BrowserAudio>(osfui::browser::msg::ToJson(wire));
    CHECK(copy.expiresMs==321 && copy.scene==encoded);
    std::cout << "browser_audio_tests: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures;
}
