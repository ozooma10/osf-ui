#include "CapturePerformance.h"
#include "check.h"
#include <cstdio>

using osfui::webview2::CapturePerformance;

int main()
{
	CapturePerformance p("test", 0);
	p.Capture(100);
	CHECK(p.Snapshot(100)["capture"]["totalFrames"] == 0);
	p.SetActive(true, 100);
	for (int i = 1; i <= 60; ++i) {
		const double now = 100 + i * (1000.0 / 60);
		p.Capture(now); p.Published(now + .1);
	}
	auto s = p.Snapshot(1100.1);
	CHECK(std::abs(s["capture"]["fps"].get<double>() - 60) < .01);
	CHECK(std::abs(s["capture"]["p95GapMs"].get<double>() - 1000.0 / 60) < .001);
	CHECK(s["scrollHitches"] == 0);
	// Damage-driven idle gaps must not be called scrolling hitches.
	p.Capture(2000); p.Published(2001);
	CHECK(p.Snapshot(2002)["scrollHitches"] == 0);
	p.Scrolling(true, 2003);
	p.Capture(2010); p.Published(2011);
	p.Capture(2100); p.Published(2110);
	p.Scrolling(true, 2111);
	p.Deferred(); p.Deferred();
	s = p.Snapshot(2111);
	CHECK(s["scrollHitches"] == 2);
	CHECK(s["hitches"][0]["stage"] == "capture");
	CHECK(s["hitches"][0]["gapMs"] == 90);
	CHECK(s["hitches"][1]["stage"] == "publication");
	CHECK(s["hitches"][1]["gapMs"] == 99);
	CHECK(s["queueDeferrals"] == 2);
	// No arriving frames: rate decays to zero, and the still-open gap is visible.
	s = p.Snapshot(8000);
	CHECK(s["capture"]["fps"] == 0);
	CHECK(s["capture"]["sinceLastMs"] == 5900);
	CHECK(s["capture"]["p95GapMs"].is_null());
	p.Capture(8001); // Expired scrolling lease: no invented hitch.
	CHECK(p.Snapshot(8002)["scrollHitches"] == 2);
	p.SetActive(false, 8100);
	p.Capture(15000); p.Published(15001); p.Deferred();
	s = p.Snapshot(16000);
	CHECK(s["capture"]["fps"].is_null());
	CHECK(s["queueDeferrals"] == 2);
	CHECK(s["activeMs"] == 8000);
	p.SetActive(true, 20000);
	p.Scrolling(true, 20000);
	p.Capture(20010); p.Published(20011);
	CHECK(p.Snapshot(20012)["scrollHitches"] == 2);
	CHECK(p.Snapshot(20012)["capture"]["p95GapMs"].is_null());
	// A bounded hitch ring retains the newest evidence; exact totals survive wrap.
	for (int i = 1; i <= 100; ++i) {
		const double now = 20010 + i * 100;
		p.Scrolling(true, now - 10);
		p.Capture(now); p.Published(now + 1);
		p.Scrolling(true, now + 2);
	}
	s = p.Snapshot(30012);
	CHECK(s["scrollHitches"] == 202);
	CHECK(s["hitches"].size() == 64);
	CHECK(s["hitchesOmitted"] == 138);
	CHECK(s["hitches"].back()["atMs"] == 30011);
	CapturePerformance fast("fast", 0);
	fast.SetActive(true, 0);
	for (int i = 1; i <= 3000; ++i) fast.Capture(i * 10);
	s = fast.Snapshot(30000);
	CHECK(s["capture"]["frames"] == 500);
	CHECK(s["capture"]["fps"] == 100);
	CHECK(s["capture"]["totalFrames"] == 3000);
	CapturePerformance idleTail("idle-tail", 0);
	idleTail.SetActive(true, 0);
	idleTail.Scrolling(true, 0);
	idleTail.Capture(10); idleTail.Published(11);
	idleTail.Capture(160); idleTail.Published(161);
	idleTail.Scrolling(true, 170, 150); // Last real movement was at 20 ms.
	CHECK(idleTail.Snapshot(170)["scrollHitches"] == 0);
	idleTail.Scrolling(false, 250, 230);
	CHECK(idleTail.Snapshot(250)["scrollHitches"] == 0);
	CHECK(idleTail.Snapshot(250)["unconfirmedScrollGaps"] == 0);
	std::printf("Capture performance: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
