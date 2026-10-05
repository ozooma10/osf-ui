#pragma once

#include "API/PapyrusApi.h"
#include "Render/WebView2HostWebRenderer.h"
#include "SettingsServices.h"

#include <deque>
#include <map>

// These are external inputs/observations, never a second implementation of Runtime.
namespace RuntimeTest
{
    struct SentMessage
    {
        std::string view;
        nlohmann::json body;
    };

    struct Environment
    {
        SettingsTest::Settings settings;
        SettingsTest::Diagnostics diagnostics;
        std::filesystem::path dataDir;
        bool rendererInitializes = true;
        bool menuEventsInstall = true;
        bool focusMenuRegisters = true;
        bool inputHookInstalls = true;
        bool transition = false;
        bool engineFocus = false;
        bool browserFocus = false;
        bool gameWindowActive = true;
        bool rendererWindowActive = true;
        bool controlsBlocked = false;
        bool cursorFree = false;
        bool paused = false;
        bool compositorVisible = false;
        bool preparedFrameReady = false;
        bool gameDeviceHealthy = true;
        int rendererInitializations = 0;
        int compositorCreations = 0;
        int filePreparations = 0;
        int papyrusInstalls = 0;
        int menuEventInstalls = 0;
        int focusRegistrations = 0;
        int inputHookInstallations = 0;
        int focusOpenRequests = 0;
        int focusCloseRequests = 0;
        int restarts = 0;
        int deviceReleases = 0;
        int queueReleases = 0;
        std::string inputTarget;
        std::vector<std::string> navigations;
        std::map<std::string, bool> hidden;
        std::vector<std::uint32_t> keys;
        std::vector<bool> backWindowActive;
        std::vector<SentMessage> sent;
        OSFUI::WebView2HostWebRenderer::LoadHandler load;
        OSFUI::WebView2HostWebRenderer::FailureHandler failure;
        OSFUI::WebView2HostWebRenderer::WebMessageHandler message;
        std::deque<std::function<void()>> notifications;
        OSFUI::API::Papyrus::PendingBatch papyrus;
    };

    extern Environment env;
    void QueueLoad(std::string view);
    void QueueHello(std::string view);
    void QueueFailure(std::string stage, std::uint32_t code = 0);
}
