#pragma once

#include "BrowserContentPolicy.h"
#include "include/cef_request_context.h"
#include "include/cef_request_context_handler.h"
#include <atomic>
#include <functional>
#include <memory>

namespace osfui::cefhost
{
    // The same policy handles page requests and requests without a browser
    // (notably service workers). It never supplies the local asset handler.
    class RemoteRequest final : public CefResourceRequestHandler
    {
    public:
        explicit RemoteRequest(std::shared_ptr<std::atomic_bool> ready) : ready_(std::move(ready)) {}
        ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
            CefRefPtr<CefRequest> request, CefRefPtr<CefCallback>) override
        { return ready_->load() && browser::IsAllowedContentResource(true, request->GetURL().ToWString()) ? RV_CONTINUE : RV_CANCEL; }
        void OnResourceRedirect(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
            CefRefPtr<CefResponse>, CefString& newUrl) override
        {
            // CEF does not repeat OnBeforeResourceLoad for redirects. There is
            // no cancel argument here: replace rejected redirects with a local
            // blank destination, before the network follows the redirect.
            if (!ready_->load() || !browser::IsAllowedContentResource(true, newUrl.ToWString())) newUrl = "about:blank";
        }
        void OnProtocolExecution(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>,
            bool& allowOS) override { allowOS = false; }
    private:
        const std::shared_ptr<std::atomic_bool> ready_;
        IMPLEMENT_REFCOUNTING(RemoteRequest);
    };

    class RemoteContext final : public CefRequestContextHandler
    {
    public:
        RemoteContext(std::function<void(std::string)> fail, std::function<void()> ready) :
            fail_(std::move(fail)), initialized_(std::move(ready)) {}
        void OnRequestContextInitialized(CefRefPtr<CefRequestContext> context) override
        {
            auto disabled = CefValue::Create();
            disabled->SetBool(false);
            for (const auto* preference : { "credentials_enable_service", "credentials_enable_autosignin",
                "autofill.profile_enabled", "autofill.credit_card_enabled" }) {
                CefString error;
                if (!context->SetPreference(preference, disabled, error)) {
                    fail_(std::string("Cannot disable ") + preference + ": " + error.ToString());
                    return;
                }
            }
            ready_->store(true);
            initialized_();
        }
        CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
            CefRefPtr<CefRequest> request, bool, bool, const CefString&, bool& disableDefault) override
        {
            disableDefault = !ready_->load() || !browser::IsAllowedContentResource(true, request->GetURL().ToWString());
            return new RemoteRequest(ready_);
        }
    private:
        const std::function<void(std::string)> fail_;
        const std::function<void()> initialized_;
        const std::shared_ptr<std::atomic_bool> ready_ = std::make_shared<std::atomic_bool>(false);
        IMPLEMENT_REFCOUNTING(RemoteContext);
    };
}
