#include "Diagnostics/HealthRegistry.h"
#include "check.h"

int main()
{
	using namespace OSFUI;
	HealthRegistry registry;
	HealthRegistry::IssueSpec failure{
		.id = "view.load-failed:demo/panel", .code = "view.load-failed",
		.severity = HealthRegistry::Severity::Error, .subject = "demo/panel",
		.context = {{ "errorCode", 10 }}
	};
	CHECK(registry.Upsert(failure));
	CHECK(!registry.Upsert(failure));
	CHECK(registry.ActiveIssues().size() == 1);
	failure.context["errorCode"] = 11;
	CHECK(registry.Upsert(failure));
	CHECK(registry.ActiveIssues()[0].context.at("errorCode") == 11);
	CHECK(registry.IsActive(failure.id));
	CHECK(registry.Resolve(failure.id));
	CHECK(!registry.Resolve(failure.id));
	CHECK(registry.ActiveIssues().empty()); // Settings owns presentation; no resolved history in UI.
	CHECK(registry.Upsert(failure));
	CHECK(!registry.Upsert({ .id = "", .code = "missing" }));
	CHECK(!registry.Upsert({ .id = "missing", .code = "" }));
	return g_failures;
}
