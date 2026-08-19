#include "Wv2Pipe.h"
#include "Wv2Protocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include "check.h"

using osfui::wv2::Pipe;
using namespace std::chrono_literals;

namespace
{
	std::atomic_uint64_t g_serial{ 0 };

	std::wstring NextPipeName()
	{
		return L"osfui-wv2-pipe-test-" + std::to_wstring(::GetCurrentProcessId()) +
			L"-" + std::to_wstring(++g_serial);
	}

	bool ConnectPair(Pipe& a_server, Pipe& a_client)
	{
		a_server.PrepareForOpen();
		a_client.PrepareForOpen();
		const auto name = NextPipeName();
		if (!a_server.CreateServer(name)) return false;
		CHECK(a_server.IsCreated());
		CHECK(!a_server.IsOpen());
		bool accepted = false;
		std::thread accept([&] {
			accepted = a_server.WaitForClient(5000);
		});
		const bool connected = a_client.Connect(name, 5000);
		accept.join();
		const bool open = connected && accepted && a_server.IsOpen() && a_client.IsOpen();
		if (!open) {
			std::fprintf(stderr,
				"ConnectPair failed: client=%d server=%d clientOpen=%d serverOpen=%d "
				"clientError='%s' serverError='%s'\n",
				connected, accepted, a_client.IsOpen(), a_server.IsOpen(),
				a_client.LastErrorText().c_str(), a_server.LastErrorText().c_str());
		}
		return open;
	}

	bool CloseWithin(Pipe& a_pipe, std::chrono::milliseconds a_limit)
	{
		const auto started = std::chrono::steady_clock::now();
		a_pipe.Close();
		return std::chrono::steady_clock::now() - started < a_limit;
	}
}

int main()
{
	const HANDLE watchdogDone = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	std::thread([watchdogDone] {
		if (::WaitForSingleObject(watchdogDone, 15000) == WAIT_TIMEOUT) {
			::TerminateProcess(::GetCurrentProcess(), 124);
		}
		::CloseHandle(watchdogDone);
	}).detach();

	for (int i = 0; i < 32; ++i) {
		Pipe server;
		server.PrepareForOpen();
		bool accepted = true;
		const auto name = NextPipeName();
		std::thread accept([&] {
			accepted = server.CreateServerAndWait(name, 5000);
		});
		CHECK(CloseWithin(server, 2s));
		accept.join();
		CHECK(!accepted);
	}

	{
		Pipe server;
		server.PrepareForOpen();
		const auto name = NextPipeName();
		CHECK(server.CreateServer(name));
		CHECK(server.IsCreated());
		CHECK(!server.IsOpen());
		bool accepted = true;
		std::thread accept([&] {
			accepted = server.WaitForClient(5000);
		});
		std::this_thread::sleep_for(25ms);
		CHECK(CloseWithin(server, 2s));
		accept.join();
		CHECK(!accepted);
	}

	Pipe server;
	Pipe client;
	CHECK(ConnectPair(server, client));
	CHECK(server.ClientProcessId() == ::GetCurrentProcessId());
	CHECK(client.ServerProcessId() == ::GetCurrentProcessId());
	CHECK(server.WriteMessage("round-trip"));
	std::string payload;
	CHECK(client.ReadMessage(payload));
	CHECK(payload == "round-trip");

	{
		Pipe timeoutServer;
		Pipe silentClient;
		CHECK(ConnectPair(timeoutServer, silentClient));
		const auto started = std::chrono::steady_clock::now();
		std::string ignored;
		CHECK(!timeoutServer.ReadMessage(ignored, 75));
		const auto elapsed = std::chrono::steady_clock::now() - started;
		CHECK(elapsed >= 50ms);
		CHECK(elapsed < 1s);
		CHECK(timeoutServer.LastErrorText().find("258") != std::string::npos);
		CHECK(CloseWithin(timeoutServer, 2s));
		silentClient.Close();
	}
	// A parked reader must finish before Close releases its pipe and event handles.
	bool read = true;
	std::thread reader([&] {
		std::string ignored;
		read = server.ReadMessage(ignored);
	});
	std::this_thread::sleep_for(25ms);
	CHECK(CloseWithin(server, 2s));
	reader.join();
	CHECK(!read);
	client.Close();

	CHECK(ConnectPair(server, client));
	bool wrote = true;
	bool queuedWrote = true;
	std::thread writer([&] {
		wrote = server.WriteMessage(
			std::string(osfui::wv2::kMaxMessageBytes, 'x'));
	});
	std::this_thread::sleep_for(25ms);
	std::thread queuedWriter([&] {
		queuedWrote = server.WriteMessage("queued");
	});
	std::this_thread::sleep_for(25ms);
	CHECK(CloseWithin(server, 2s));
	writer.join();
	queuedWriter.join();
	CHECK(!wrote);
	CHECK(!queuedWrote);
	client.Close();

	// The same Pipe objects can begin a clean recovery session after Close.
	CHECK(ConnectPair(server, client));
	CHECK(client.WriteMessage("reopened"));
	payload.clear();
	CHECK(server.ReadMessage(payload));
	CHECK(payload == "reopened");
	server.Close();
	client.Close();

	::SetEvent(watchdogDone);
	std::fprintf(stderr, "wv2_pipe_tests: %d checks, %d failure(s)\n",
		g_checks, g_failures);
	return g_failures;
}
