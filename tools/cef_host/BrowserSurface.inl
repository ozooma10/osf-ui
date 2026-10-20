        browser::PanelRect ViewBounds(const View& view) const
        {
            if (!browserLayout) return {0,0,static_cast<int>(viewportWidth),static_cast<int>(viewportHeight)};
            return browser::BrowserPanelRect(viewportWidth,viewportHeight,browserLayout->chromeHeight,
                browserLayout->fullscreen,browserLayout->landing,
                view.id == browserLayout->chrome ? browser::BrowserPanel::Chrome : browser::BrowserPanel::Content);
        }
        void BrowserLayout(const msg::BrowserLayout& layout)
        {
            const auto chrome=views.find(layout.chrome), content=views.find(layout.content);
            if (layout.chrome != browser::kBrowserView || chrome == views.end() || chrome->second->remoteContent ||
                (!layout.content.empty() && (layout.content != browser::kBrowserContent ||
                    content == views.end() || !content->second->remoteContent))) {
                Fatal("cef-browser-layout",E_ACCESSDENIED,"Invalid browser pair"); return;
            }
            browserPointer.Reset(); pointerTarget.clear();
            browserLayout=layout; epoch=layout.presentationEpoch;
#ifdef OSFUI_CEF_AUDIO_PROBE
            { std::lock_guard lock(audioScene->mutex); audioScene->fullscreen = layout.fullscreen; }
#endif
            for (auto& [id, view] : views) {
                view->hidden = id == layout.chrome ? !(layout.fullscreen || layout.landing) : layout.landing;
                view->browser->GetHost()->WasHidden(view->hidden);
            }
            if (!Input()) inputTarget = layout.fullscreen || layout.landing ? layout.chrome : layout.content;
            InvalidateGeometry(); ReconcileFocus();
        }
        bool BrowserFocusShortcut(const msg::Keyboard& key)
        {
            if (!browserLayout || !focus || !windowActive ||
                !(key.vk == VK_F6 || (key.vk == 'L' && (key.modifiers & 2)))) return false;
            const auto chrome=views.find(browserLayout->chrome);
            if (key.down && !key.repeat && chrome != views.end() && !chrome->second->hidden) {
                inputTarget=chrome->first; ReconcileFocus();
                chrome->second->Deliver(Json::Dump(json{{"kind","event"},{"name","browser.focusAddress"},{"payload",json::object()}}));
            }
            return true;
        }
        bool BrowserBack(const msg::Back& request)
        {
            if (!browserLayout || request.view != browserLayout->chrome) return false;
            if (!focus || !windowActive || request.presentationEpoch != epoch) return true;
            const auto content=views.find(browserLayout->content);
            if (content != views.end() && content->second->browser->GetHost()->IsFullscreen())
                content->second->browser->GetHost()->ExitFullscreen(false);
            else Send(msg::ToJson(msg::BackUnhandled{.view=request.view,.presentationEpoch=request.presentationEpoch}));
            return true;
        }
        void BrowserCommand(const msg::BrowserCommand& command)
        {
            msg::BrowserResult result{.token=command.token};
            const auto found=views.find(command.view);
            if (!init.remoteMode || found == views.end() || !found->second->remoteContent || found->second->closing) {
                result.error="Browser content is unavailable"; Send(msg::ToJson(result)); return;
            }
            auto& view=*found->second;
            auto web=view.browser;
            result.success=true;
            if (command.action == "navigate" &&
                browser::IsAllowedContentDocument(true,ToWide(command.url),{},{})) web->GetMainFrame()->LoadURL(command.url);
            else if (command.action == "back") web->GoBack();
            else if (command.action == "forward") web->GoForward();
            else if (command.action == "reload") web->Reload();
            else if (command.action == "stop") web->StopLoad();
            else if (command.action == "mute") {
                web->GetHost()->SetAudioMuted(command.value);
#ifdef OSFUI_CEF_AUDIO_PROBE
                std::lock_guard lock(audioScene->mutex);
                audioScene->muted = command.value;
#endif
            }
            else if (command.action == "clearData") {
                // CEF cannot clear all storage APIs reliably while the context is open.
                // Delete only this host's two cache directories before its next CefInitialize.
                std::ofstream marker(std::filesystem::path(ToWide(init.userDataDir)) / "clear-browser-data",std::ios::trunc);
                marker << "1\n"; marker.flush();
                result.success=marker.good(); result.restartRequired=result.success;
            } else result.success=false;
            if (!result.success) result.error="Browser command failed";
            Send(msg::ToJson(result)); view.PublishBrowserState();
        }
