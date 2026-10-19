#pragma once

// Explicit real-helper acceptance; the ordinary performance workloads do not run these.
void Bench::RuntimeChecks()
{
    phase=runtimeCheck;
    auto require=[&](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    auto command=[&](const std::string& view,const std::string& test,json payload=json::object()){
        payload["test"]=test;
        Send(msg::PostWeb{.view=view,.json=json{{"kind","event"},{"name","runtime.command"},{"payload",payload}}.dump()});
    };
    auto response=[&](const std::string& view,const std::string& test){
        const auto key=view+":"+test;
        WaitUntil([&]{return runtimeReplies.contains(key);},15,"runtime page result");
        const auto answer=runtimeReplies.at(key);result["runtime_checks"].push_back(answer);
        if(!answer.value("passed",false))throw std::runtime_error("page check failed: "+answer.dump());
        return answer;
    };
    if(runtimeCheck=="gpu"){
        command("bench/main","gpu");const auto answer=response("bench/main","gpu");
        require(answer.at("renderer").get<std::string>().find(consumer.adapterName)!=std::string::npos,
            "Chromium ANGLE renderer does not match the requested DXGI adapter");
        consumer.Save(output/"gpu.png");
    }else if(runtimeCheck=="storage-write"||runtimeCheck=="storage-read"){
        require(!storageToken.empty(),"storage token required");
        command("bench/main",runtimeCheck,{{"token",storageToken}});
        response("bench/main",runtimeCheck);
        // Return immediately after the committed write; Stop closes the pipe without a flush delay.
    }else if(runtimeCheck=="media"){
        command("bench/main","media");response("bench/main","media");
    }else if(runtimeCheck=="isolation"){
        measuring=true;command("bench/main","animate");response("bench/main","animate");RunFor(.2);
        auto progress=[&](const char* name,bool partial){
            const auto before=unique,partialBefore=incompleteFrames;
            RunFor(1);
            const auto changed=unique-before,partials=incompleteFrames-partialBefore;
            result["isolation"].push_back({{"phase",name},{"fresh_main_frames",changed},{"partial_frames",partials}});
            require(changed>=5,"healthy view stopped producing fresh pixels");
            if(partial)require(partials>=5,"unready sibling was not represented by partial frames");
            require(invalid==0,"healthy view marker was missing or corrupted");
        };
        progress("baseline",false);
        Send(msg::Navigate{.id="bench/second",.entry="slow.html",.logicalHeight=height});
        Send(msg::SetHidden{.view="bench/second",.hidden=false,.presentationEpoch=2});
        response("bench/second","slow-start");
        for(int i=0;i<128;++i)command("bench/second","queue",{{"sequence",i}});
        progress("slow-load-and-pending-message-overflow",true);
        require(!loadedViews.contains("bench/second"),"slow load ended before isolation was measured");
        WaitUntil([&]{return loadedViews.contains("bench/second")&&lastComplete;},10,"slow sibling reveal");
        command("bench/second","queue-result");const auto queue=response("bench/second","queue-result");
        require(queue.at("last")==127&&queue.at("count")==64,"pending queue did not retain the newest 64 messages");
        progress("recovered-sibling",false);

        loadedViews.erase("bench/second");
        Send(msg::Navigate{.id="bench/second",.entry="missing.html",.logicalHeight=height});
        WaitUntil([&]{return failedViews.contains("bench/second");},10,"expected sibling load error");
        progress("failed-load",true);
        failedViews.erase("bench/second");
        Send(msg::Navigate{.id="bench/second",.entry="missed.html",.logicalHeight=height});
        WaitUntil([&]{return loadedViews.contains("bench/second");},10,"loaded sibling with missed reveal");
        progress("missed-reveal",true);
        consumer.Save(output/"isolated-view.png");

        loadedViews.erase("bench/second");
        Send(msg::Navigate{.id="bench/second",.entry="index.html",.logicalHeight=height});
        WaitUntil([&]{return loadedViews.contains("bench/second")&&lastComplete;},10,"sibling retry reveal");
        progress("retry-restores-complete-frames",false);
        require(debugPort!=0,"renderer-crash check requires a private debugging port");
        // The supervisor targets only this helper's second page through its private CDP port.
        std::ofstream(output/"crash-ready.json")<<json{{"pid",processes.root},{"port",debugPort}}.dump();
        WaitUntil([&]{return failedViews.contains("bench/second");},15,"owned sibling renderer crash");
        progress("renderer-crash",true);
        loadedViews.erase("bench/second");failedViews.erase("bench/second");
        Send(msg::Navigate{.id="bench/second",.entry="index.html",.logicalHeight=height});
        WaitUntil([&]{return loadedViews.contains("bench/second")&&lastComplete;},10,"renderer crash recovery");
        progress("renderer-recovered",false);
    }else throw std::runtime_error("unknown runtime check");
    std::cout<<"passed: "<<runtimeCheck<<" on "<<consumer.adapterName<<std::endl;
}
