#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include "BrowserPipe.h"
#include "BrowserMessages.h"
#include "Win32Util.h"

using Microsoft::WRL::ComPtr;
using json=nlohmann::json;
namespace msg=osfui::browser::msg;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
double Seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
void Check(HRESULT hr,const char* what) { if(FAILED(hr)) throw std::runtime_error(std::string(what)+" HRESULT="+std::to_string(static_cast<UINT>(hr))); }
double Quantile(std::vector<double> v,double p) { if(v.empty())return 0;std::sort(v.begin(),v.end());return v[static_cast<size_t>((v.size()-1)*p)]; }
double Mean(const std::vector<double>& v) { double sum=0;for(double n:v)sum+=n;return v.empty()?0:sum/v.size(); }
UINT64 FileTicks(FILETIME t) { return (static_cast<UINT64>(t.dwHighDateTime)<<32)|t.dwLowDateTime; }

struct Processes {
    DWORD root{};
    std::map<DWORD,HANDLE> handles;
    PDH_HQUERY query{}; PDH_HCOUNTER gpu{};
    Processes(){if(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS){if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&gpu)!=ERROR_SUCCESS)gpu=nullptr;PdhCollectQueryData(query);}}
    ~Processes(){for(auto [id,h]:handles)CloseHandle(h);if(query)PdhCloseQuery(query);}
    void Discover(){
        HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snap==INVALID_HANDLE_VALUE)return;
        PROCESSENTRY32W e{sizeof(e)};std::vector<std::pair<DWORD,DWORD>> all;
        if(Process32FirstW(snap,&e))do{all.emplace_back(e.th32ProcessID,e.th32ParentProcessID);}while(Process32NextW(snap,&e));CloseHandle(snap);
        std::set<DWORD> ids{root};bool changed=true;
        while(changed){changed=false;for(auto [id,parent]:all)if(ids.contains(parent)&&!ids.contains(id)){ids.insert(id);changed=true;}}
        for(DWORD id:ids)if(!handles.contains(id)){auto h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,id);if(h)handles[id]=h;}
    }
    json Sample(){
        Discover();double cpu=0,rootCpu=0,commit=0,working=0;UINT live=0;json perProcess=json::object();
        for(auto [id,h]:handles){FILETIME create{},exit{},kernel{},user{};if(GetProcessTimes(h,&create,&exit,&kernel,&user)){const auto seconds=(FileTicks(kernel)+FileTicks(user))/1e7;cpu+=seconds;if(id==root)rootCpu=seconds;perProcess[std::to_string(id)]["cpu_seconds"]=seconds;}
            if(WaitForSingleObject(h,0)==WAIT_TIMEOUT){live++;PROCESS_MEMORY_COUNTERS_EX m{};if(GetProcessMemoryInfo(h,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))){commit+=m.PrivateUsage;working+=m.WorkingSetSize;}}}
        double gpuTotal=0;bool gpuValid=false;
        if(gpu&&PdhCollectQueryData(query)==ERROR_SUCCESS){DWORD bytes=0,count=0;if(PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,nullptr)==PDH_MORE_DATA){
            std::vector<unsigned char> memory(bytes);auto data=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(memory.data());
            if(PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,data)==ERROR_SUCCESS){for(DWORD i=0;i<count;i++){
                DWORD pid=0;if(swscanf_s(data[i].szName,L"pid_%lu_",&pid)==1&&handles.contains(pid)&&data[i].FmtValue.CStatus==PDH_CSTATUS_VALID_DATA){gpuTotal+=data[i].FmtValue.doubleValue;gpuValid=true;}
            }}}}
        return {{"cpu_seconds",cpu},{"root_cpu_seconds",rootCpu},{"per_process",perProcess},{"private_commit_mib",commit/1048576},{"working_set_mib",working/1048576},{"processes",live},{"gpu_engine_sum_percent",gpuValid?json(gpuTotal):json(nullptr)}};
    }
};

struct Consumer {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11Device1> device1;ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> produce,done;std::vector<ComPtr<ID3D11Texture2D>> slots;
    ComPtr<ID3D11Texture2D> owned,pixel;HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
    LUID adapter{};std::string adapterName;UINT width{},height{};
    ~Consumer(){if(event)CloseHandle(event);}
    Consumer(){
        ComPtr<IDXGIFactory6> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        ComPtr<IDXGIAdapter1> selected;Check(factory->EnumAdapterByGpuPreference(0,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&selected)),"GPU selection");
        DXGI_ADAPTER_DESC1 desc{};selected->GetDesc1(&desc);adapter=desc.AdapterLuid;adapterName=osfui::win32::ToUtf8(desc.Description);
        Check(D3D11CreateDevice(selected.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11 consumer");
        Check(device.As(&device1),"device1");Check(device.As(&device5),"device5");Check(context.As(&context4),"context4");
        Check(device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"completion fence");
    }
    void Ring(const msg::Textures& ring){
        slots.clear();produce.Reset();owned.Reset();pixel.Reset();width=ring.width;height=ring.height;
        for(auto value:ring.slots){HANDLE h=reinterpret_cast<HANDLE>(value);ComPtr<ID3D11Texture2D> t;auto hr=device1->OpenSharedResource1(h,IID_PPV_ARGS(&t));CloseHandle(h);Check(hr,"open shared slot");slots.push_back(t);}
        HANDLE h=reinterpret_cast<HANDLE>(ring.produceFence);auto hr=device5->OpenSharedFence(h,IID_PPV_ARGS(&produce));CloseHandle(h);Check(hr,"open produce fence");
        D3D11_TEXTURE2D_DESC desc{};slots.at(0)->GetDesc(&desc);desc.MiscFlags=0;desc.BindFlags=0;
        Check(device->CreateTexture2D(&desc,nullptr,&owned),"owned consumer texture");
        desc.Width=desc.Height=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(device->CreateTexture2D(&desc,nullptr,&pixel),"marker staging");
    }
    UINT Consume(const msg::Frame& frame){
        if(frame.slot>=slots.size()||!produce)throw std::runtime_error("frame before ring");
        Check(context4->Wait(produce.Get(),frame.serial),"wait produce");
        context->CopyResource(owned.Get(),slots[frame.slot].Get());
        const D3D11_BOX box{8,8,0,9,9,1};context->CopySubresourceRegion(pixel.Get(),0,0,0,0,owned.Get(),0,&box);
        Check(context4->Signal(done.Get(),++serial),"signal consumer");context->Flush();
        if(done->GetCompletedValue()<serial){Check(done->SetEventOnCompletion(serial,event),"consumer event");if(WaitForSingleObject(event,3000)!=WAIT_OBJECT_0)throw std::runtime_error("consumer GPU timeout");}
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(pixel.Get(),0,D3D11_MAP_READ,0,&mapped),"marker read");
        auto* p=static_cast<unsigned char*>(mapped.pData);const UINT value=(p[3]==255)?(p[2]<<16)|(p[1]<<8)|p[0]:0;context->Unmap(pixel.Get(),0);return value;
    }
    void Save(const fs::path& path){
        if(!owned)return;
        D3D11_TEXTURE2D_DESC desc{};owned->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&desc,nullptr,&staging),"screenshot staging");context->CopyResource(staging.Get(),owned.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"screenshot read");
        ComPtr<IWICImagingFactory> factory;Check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
        ComPtr<IWICStream> stream;Check(factory->CreateStream(&stream),"WIC stream");Check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG file");
        ComPtr<IWICBitmapEncoder> encoder;Check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");Check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG init");
        ComPtr<IWICBitmapFrameEncode> image;Check(encoder->CreateNewFrame(&image,nullptr),"PNG frame");Check(image->Initialize(nullptr),"PNG frame init");Check(image->SetSize(width,height),"PNG size");
        auto format=GUID_WICPixelFormat32bppBGRA;Check(image->SetPixelFormat(&format),"PNG format");Check(image->WritePixels(height,mapped.RowPitch,mapped.RowPitch*height,static_cast<BYTE*>(mapped.pData)),"PNG pixels");
        Check(image->Commit(),"PNG commit");Check(encoder->Commit(),"PNG finish");context->Unmap(staging.Get(),0);
    }
};

struct Bench {
    osfui::browser::Pipe pipe;Consumer consumer;Processes processes;
    HANDLE process{},job{},wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);HWND window{};
    std::thread reader;std::atomic_bool disconnected{};std::mutex mutex;
    struct Received{json value;double time;};std::deque<Received> messages;
    osfui::browser::Backend backend = osfui::browser::Backend::WebView2;
    fs::path output,assets,profile,host;UINT width=1920,height=1080;double duration=10,warmup=3,start{},firstFrame{},firstContent{},loaded{};
    json result={{"phases",json::array()}};std::ofstream events;
    std::set<std::string> ready,modeReady;std::string phase;bool measuring=false,allowNoOutput=false,lifecycle=false,holdAcks=false;
    std::vector<msg::FrameAck> heldAcks;UINT64 lastEpoch{};
    UINT lastMarker{};double previousFrame{},previousUnique{};size_t frames{},unique{},invalid{};
    std::vector<double> intervals,uniqueIntervals,consumeTimes,pingTimes;
    std::map<int,double> pings;int nextPing{};std::vector<json> pageStats;
    ~Bench(){Stop();if(window)DestroyWindow(window);if(wake)CloseHandle(wake);}
    void Send(const json& value){if(pipe.WriteMessage(value.dump())!=osfui::browser::Pipe::WriteResult::Written)throw std::runtime_error("pipe write failed");}
    template<class T>void Send(const T& value){Send(msg::ToJson(value));}
    void Start(){
        fs::create_directories(output);fs::create_directories(profile);events.open(output/"events.jsonl");
        window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"OSF UI benchmark consumer",WS_POPUP,-32000,-32000,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        const auto pipeName=L"osfui-bench-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());pipe.PrepareForOpen();
        if(!pipe.CreateServer(pipeName))throw std::runtime_error(pipe.LastErrorText());
        auto command=L"\""+host.wstring()+L"\" --pipe="+pipeName+L" --game-pid="+std::to_wstring(GetCurrentProcessId())+L" --log=\""+(output/L"host.log").wstring()+L"\"";
        STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
        job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limit,sizeof(limit)))throw std::runtime_error("create owned benchmark job");
        start=Seconds();if(!CreateProcessW(host.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,host.parent_path().c_str(),&si,&pi))throw std::runtime_error("launch failed: "+std::to_string(GetLastError()));
        process=pi.hProcess;processes.root=pi.dwProcessId;
        if(!AssignProcessToJobObject(job,process)){TerminateProcess(process,1);CloseHandle(pi.hThread);throw std::runtime_error("assign benchmark job");}
        ResumeThread(pi.hThread);CloseHandle(pi.hThread);
        if(!pipe.WaitForClient(20000))throw std::runtime_error("host did not connect: "+pipe.LastErrorText());
        reader=std::thread([this]{std::string payload;while(pipe.ReadMessage(payload)){auto value=json::parse(payload,nullptr,false);if(value.is_discarded())continue;const auto now=Seconds();{std::lock_guard lock(mutex);messages.push_back({std::move(value),now});}SetEvent(wake);}disconnected=true;SetEvent(wake);});
        result["adapter"]=consumer.adapterName;result["width"]=width;result["height"]=height;result["pid"]=pi.dwProcessId;
        result["consumer"]="D3D11 GPU copy plus 1-pixel frame marker readback; acknowledgements after GPU completion";
        Send(msg::Init{.topLevelHwnd=reinterpret_cast<UINT64>(window),.viewsPath=osfui::win32::ToUtf8(assets.wstring()),.legacyViewsPath=osfui::win32::ToUtf8(assets.wstring()),.width=width,.height=height,.userDataDir=osfui::win32::ToUtf8(profile.wstring()),.devMode=false,.language="en",.adapterLuidLow=consumer.adapter.LowPart,.adapterLuidHigh=static_cast<UINT>(consumer.adapter.HighPart)});
        Send(msg::WindowActive{.active=true});Send(msg::Viewport{.width=width,.height=height,.presentationEpoch=1});
        Send(msg::Navigate{.id="bench/main",.entry="index.html",.logicalHeight=height});
        Send(msg::SetHidden{.view="bench/main",.hidden=false,.presentationEpoch=1});
        WaitUntil([this]{return ready.contains("bench/main")&&firstContent>0;},30,"first content frame");
        result["startup_ms"]={{"hello",result.value("hello_ms",0.0)},{"load",(loaded-start)*1000},{"first_frame",(firstFrame-start)*1000},{"first_content",(firstContent-start)*1000}};
    }
    void Stop(){
        if(!process)return;const auto at=Seconds();pipe.Close();if(reader.joinable())reader.join();
        const bool exited=WaitForSingleObject(process,GetEnvironmentVariableW(L"OSFUI_CEF_AUDIT_TRACE",nullptr,0)&&GetEnvironmentVariableW(L"OSFUI_CEF_AUDIT_MODE",nullptr,0)?35000:7000)==WAIT_OBJECT_0;DWORD code=0;GetExitCodeProcess(process,&code);
        result["shutdown_ms"]=(Seconds()-at)*1000;result["shutdown_clean"]=exited&&code==0;result["exit_code"]=code;
        if(!exited||code!=0){result["status"]="failed";result["error"]="helper shutdown failed, exit="+std::to_string(code);}
        CloseHandle(job);job=nullptr;CloseHandle(process);process=nullptr;
    }
    void Drain(){
        std::deque<Received> batch;{std::lock_guard lock(mutex);batch.swap(messages);}
        for(auto& received:batch){auto& v=received.value;const auto type=v.value("type","");
            if(type!="frame"&&type!="heartbeat")events<<json{{"seconds",received.time-start},{"message",v}}.dump()<<'\n';
            if(type=="hello"){if(!msg::IsCompatibleHello(msg::FromJson<msg::Hello>(v),backend))throw std::runtime_error("browser host handshake mismatch"); result["runtime"]=v.value("runtimeVersion","");result["hello_ms"]=(received.time-start)*1000;}
            else if(type=="fatal")throw std::runtime_error("host fatal: "+v.dump());
            else if(type==msg::LoadEvent::kType){if(v.value("failed",false))throw std::runtime_error("load failure: "+v.dump());if(!loaded)loaded=received.time;}
            else if(type=="textures")consumer.Ring(msg::FromJson<msg::Textures>(v));
            else if(type=="frame"){
                auto frame=msg::FromJson<msg::Frame>(v);const auto before=Seconds();const UINT marker=consumer.Consume(frame);const auto cost=(Seconds()-before)*1000;
                const msg::FrameAck ack{.slot=frame.slot,.serial=frame.serial};
                if(holdAcks)heldAcks.push_back(ack);else Send(ack);lastEpoch=frame.presentationEpoch;if(!firstFrame)firstFrame=received.time;
                const bool valid=marker>=0x110000&&marker<0x200000;if(valid&&!firstContent)firstContent=received.time;
                if(measuring){frames++;consumeTimes.push_back(cost);if(previousFrame)intervals.push_back((received.time-previousFrame)*1000);previousFrame=received.time;
                    if(!valid){invalid++;if(lifecycle)events<<json{{"seconds",received.time-start},{"invalid_marker",marker},{"epoch",frame.presentationEpoch},{"phase",phase}}.dump()<<'\n';}else if(marker!=lastMarker){unique++;if(previousUnique)uniqueIntervals.push_back((received.time-previousUnique)*1000);previousUnique=received.time;}}
                lastMarker=marker;
            }else if(type=="webMessage"){
                const auto m=json::parse(v.value("json",""),nullptr,false);if(m.is_discarded())continue;const auto name=m.value("name","");const auto view=v.value("view","");
                if(name=="bench.ready")ready.insert(view);
                if(name=="bench.modeReady")modeReady.insert(view);
                if(name=="bench.stats"&&measuring)pageStats.push_back({{"view",view},{"payload",m.value("payload",json::object())}});
                if(name=="bench.pong"){int id=m.at("payload").at("id");auto found=pings.find(id);if(found!=pings.end()){if(measuring)pingTimes.push_back((received.time-found->second)*1000);pings.erase(found);}}
            }
        }
        MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}
        if(disconnected)throw std::runtime_error("host disconnected");
    }
    void WaitUntil(std::function<bool()> condition,double timeout,const char* what){const auto end=Seconds()+timeout;while(!condition()){if(Seconds()>end)throw std::runtime_error(std::string("timeout: ")+what);WaitForSingleObject(wake,5);Drain();}}
    void RunFor(double seconds){const auto end=Seconds()+seconds;while(Seconds()<end){WaitForSingleObject(wake,5);Drain();}}
    void SetMode(const std::string& view,const std::string& mode){Send(msg::PostWeb{.view=view,.json=json{{"kind","event"},{"name","bench.mode"},{"payload",{{"mode",mode}}}}.dump()});}
    void Lifecycle(){
        result["lifecycle"]=json::array();measuring=true;phase="backpressure";
        auto require=[this](bool ok,const char* name){if(!ok)throw std::runtime_error(name);result["lifecycle"].push_back(name);std::cout<<"passed: "<<name<<std::endl;};
        auto ping=[this]{const int id=++nextPing;pings[id]=Seconds();Send(msg::PostWeb{.view="bench/main",.json=json{{"kind","event"},{"name","bench.ping"},{"payload",{{"id",id}}}}.dump()});WaitUntil([&]{return !pings.contains(id);},3,"bridge echo");};
        modeReady.clear();SetMode("bench/main","canvas");WaitUntil([&]{return modeReady.size()==1;},5,"animation ready");RunFor(.3);
        holdAcks=true;RunFor(.4);const auto blockedFrames=frames;const auto blockedMarker=lastMarker;RunFor(.2);
        require(heldAcks.size()==osfui::browser::kRingSlots&&frames==blockedFrames,"full ring stops publication");
        ping();require(true,"messages remain responsive under backpressure");
        modeReady.clear();SetMode("bench/main","idle");WaitUntil([&]{return modeReady.size()==1;},5,"static ready");RunFor(.1);
        holdAcks=false;for(const auto& ack:heldAcks)Send(ack);heldAcks.clear();
        WaitUntil([&]{return frames>blockedFrames&&lastMarker!=blockedMarker;},3,"pending static frame after ack");
        require(true,"ack publishes latest static pixels without another paint stream");
        phase="visibility";Send(msg::SetHidden{.view="bench/main",.hidden=true,.presentationEpoch=2});RunFor(.15);const auto hiddenFrames=frames;RunFor(.2);
        require(frames==hiddenFrames,"hidden view produces no output");
        Send(msg::SetHidden{.view="bench/main",.hidden=false,.presentationEpoch=3});WaitUntil([&]{return lastEpoch==3&&frames>hiddenFrames;},5,"reshow frame");
        require(true,"reshow produces current epoch");
        for(int i=0;i<300;i++)Send(msg::SetOrder{.view="bench/main",.order=i});ping();
        require(true,"message burst drains beyond one 128-message batch");
        modeReady.clear();SetMode("bench/main","canvas");WaitUntil([&]{return modeReady.size()==1;},5,"animation restart");RunFor(.2);
        holdAcks=true;RunFor(.3);require(heldAcks.size()==osfui::browser::kRingSlots,"ring filled before resize");
        phase="resize";Send(msg::Resize{.width=1280,.height=720});Send(msg::Viewport{.width=1280,.height=720,.presentationEpoch=4});
        WaitUntil([&]{return lastEpoch==4;},5,"resize while backpressured");
        holdAcks=false;for(const auto& ack:heldAcks)Send(ack);heldAcks.clear();RunFor(.15);
        require(consumer.width==1280&&consumer.height==720,"resize replaces blocked ring");
        phase="reopen";ready.erase("bench/main");Send(msg::DestroyView{.view="bench/main"});
        Send(msg::Navigate{.id="bench/main",.entry="index.html",.logicalHeight=720});Send(msg::SetHidden{.view="bench/main",.hidden=false,.presentationEpoch=5});
        WaitUntil([&]{return ready.contains("bench/main");},5,"reopened document ready");
        // A document-ready message precedes its first paint. Require pixels from
        // this document, not a frame epoch (or a stale marker from the old view).
        constexpr UINT reopenedMarker=0x1f1234;
        Send(msg::PostWeb{.view="bench/main",.json=json{{"kind","event"},{"name","bench.marker"},{"payload",{{"serial",reopenedMarker}}}}.dump()});
        WaitUntil([&]{return lastEpoch==5&&lastMarker==reopenedMarker;},5,"reopened document pixels");
        result["transition_invalid_frames"]=invalid;
        const auto settledInvalid=invalid;RunFor(.2);
        require(invalid==settledInvalid,"destroy and reopen publishes fresh stable document pixels");
        consumer.Save(output/"reopened.png");
        SetMode("bench/main","canvas"); // Shut down with live paints and scheduled work.
        measuring=false;
    }
    void AuditPhase(bool active){if(GetEnvironmentVariableW(L"OSFUI_CEF_AUDIT_MODE",nullptr,0))Send(msg::PostWeb{.view="bench/main",.json=json{{"kind","event"},{"name","bench.audit"},{"payload",{{"phase",phase},{"measuring",active}}}}.dump()});}
    void Phase(const std::string& name,const std::string& mode,int count){
        phase=name;modeReady.clear();
        if(count>1){for(const auto* id:{"bench/second","bench/third"}){Send(msg::Navigate{.id=id,.entry="index.html",.logicalHeight=height});Send(msg::SetHidden{.view=id,.hidden=false,.presentationEpoch=1});}
            WaitUntil([this]{return ready.size()==3;},20,"three views ready");}
        for(const auto& view:ready)SetMode(view,mode);
        WaitUntil([this,count]{return modeReady.size()==count;},10,"mode ack");RunFor(warmup);
        frames=unique=invalid=0;previousFrame=previousUnique=0;intervals.clear();uniqueIntervals.clear();consumeTimes.clear();pingTimes.clear();pageStats.clear();pings.clear();
        std::vector<json> samples;auto first=processes.Sample();const double begin=Seconds(),end=begin+duration;double sampleAt=begin+1,pingAt=begin;
        AuditPhase(true);measuring=true;
        while(Seconds()<end){WaitForSingleObject(wake,5);Drain();const auto now=Seconds();
            if(now>=pingAt){const int id=++nextPing;pings[id]=Seconds();Send(msg::PostWeb{.view="bench/main",.json=json{{"kind","event"},{"name","bench.ping"},{"payload",{{"id",id}}}}.dump()});pingAt=now+.1;}
            if(now>=sampleAt){auto s=processes.Sample();s["seconds"]=Seconds()-begin;samples.push_back(s);sampleAt=now+1;}}
        measuring=false;AuditPhase(false);const auto finish=Seconds();auto last=processes.Sample();samples.push_back(last);
        std::vector<double> memory,gpu;for(auto& s:samples){memory.push_back(s.at("private_commit_mib"));if(!s["gpu_engine_sum_percent"].is_null())gpu.push_back(s["gpu_engine_sum_percent"]);}
        json row={{"name",name},{"seconds",finish-begin},{"views",count},{"frames",frames},{"unique_frames",unique},{"invalid_markers",invalid},
            {"delivered_fps",frames/(finish-begin)},{"unique_fps",unique/(finish-begin)},{"frame_interval_p50_ms",Quantile(intervals,.5)},{"frame_interval_p95_ms",Quantile(intervals,.95)},
            {"unique_interval_p95_ms",Quantile(uniqueIntervals,.95)},{"consumer_copy_p95_ms",Quantile(consumeTimes,.95)},
            {"bridge_rtt_p50_ms",Quantile(pingTimes,.5)},{"bridge_rtt_p95_ms",Quantile(pingTimes,.95)},{"bridge_replies",pingTimes.size()},
            {"cpu_core_percent",(last.at("cpu_seconds").get<double>()-first.at("cpu_seconds").get<double>())/(finish-begin)*100},
            {"root_cpu_core_percent",(last.at("root_cpu_seconds").get<double>()-first.at("root_cpu_seconds").get<double>())/(finish-begin)*100},
            {"process_baseline",first["per_process"]},{"process_final",last["per_process"]},
            {"private_commit_mean_mib",Mean(memory)},{"private_commit_peak_mib",Quantile(memory,1)},{"processes",last["processes"]},
            {"gpu_engine_sum_mean_percent",gpu.empty()?json(nullptr):json(Mean(gpu))},{"samples",samples},{"page_stats",pageStats}};
        result["diagnostic_no_output"]=allowNoOutput;
        result["phases"].push_back(row);if(!allowNoOutput)consumer.Save(output/(name+".png"));Write();
        std::cout<<name<<" fps="<<row["unique_fps"]<<" CPU="<<row["cpu_core_percent"]<<" mem="<<row["private_commit_mean_mib"]<<std::endl;
        if(invalid||(!allowNoOutput&&!unique&&mode!="idle"))throw std::runtime_error("rendered workload validation failed");
    }
    void Write(){std::ofstream(output/"result.json")<<result.dump(2);}
};

int wmain(int argc,wchar_t** argv){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try{Bench bench;
        for(int i=1;i+1<argc;i+=2){std::wstring key=argv[i],value=argv[i+1];if(key==L"--backend"){auto parsed=osfui::browser::ParseBackend(osfui::win32::ToUtf8(value));if(!parsed)throw std::runtime_error("invalid backend");bench.backend=*parsed;}else if(key==L"--host")bench.host=fs::absolute(value);else if(key==L"--out")bench.output=fs::absolute(value);else if(key==L"--views")bench.assets=fs::absolute(value);else if(key==L"--profile")bench.profile=fs::absolute(value);else if(key==L"--width")bench.width=std::stoul(value);else if(key==L"--height")bench.height=std::stoul(value);else if(key==L"--seconds")bench.duration=std::stod(value);else if(key==L"--warmup")bench.warmup=std::stod(value);else if(key==L"--allow-no-output")bench.allowNoOutput=value==L"true";else if(key==L"--lifecycle")bench.lifecycle=value==L"true";}
        if(bench.host.empty()||bench.output.empty()||bench.assets.empty()||bench.profile.empty())throw std::runtime_error("required: --host --out --views --profile");
        try{bench.Start();if(bench.lifecycle)bench.Lifecycle();else{bench.Phase("idle","idle",1);bench.Phase("canvas","canvas",1);bench.Phase("dom","dom",1);bench.Phase("three_views","canvas",3);}bench.result["status"]="passed";}
        catch(const std::exception& e){bench.result["status"]="failed";bench.result["error"]=e.what();std::cerr<<e.what()<<std::endl;}
        bench.Stop();bench.Write();return bench.result.value("status","")=="passed"?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 2;}
}
