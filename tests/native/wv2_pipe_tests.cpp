#include "BrowserPipe.h"
#include "BrowserPipeWriter.h"
#include "BrowserProtocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "check.h"

using osfui::browser::Pipe;
using osfui::browser::PipeWriter;
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

	std::unique_ptr<PipeWriter::HandleBatch> MakeHandleBatch(std::vector<HANDLE>& a_remote)
	{
		// Duplicate into this process so handle lifetime can be checked without a game/GPU.
		auto batch = std::make_unique<PipeWriter::HandleBatch>(::GetCurrentProcess(), 4);
		const HANDLE source = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		CHECK(source != nullptr);
		for (int i = 0; i < 4; ++i) {
			HANDLE remote = nullptr;
			CHECK(::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(),
				&remote, 0, FALSE, DUPLICATE_SAME_ACCESS));
			batch->Add(remote);
			a_remote.push_back(remote);
		}
		::CloseHandle(source);
		return batch;
	}

	void CheckHandles(const std::vector<HANDLE>& a_handles, bool a_open)
	{
		for (const auto handle : a_handles) {
			DWORD flags = 0;
			const bool open = ::GetHandleInformation(handle, &flags) != FALSE;
			CHECK(open == a_open);
			if (!a_open) CHECK(::GetLastError() == ERROR_INVALID_HANDLE);
		}
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
	CHECK(server.WriteMessage(std::string(osfui::browser::kMaxMessageBytes + 1, 'x')) == Pipe::WriteResult::InvalidPayload);
	CHECK(server.WriteMessage("") == Pipe::WriteResult::InvalidPayload);
	CHECK(server.WriteMessage("round-trip") == Pipe::WriteResult::Written);
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
			std::string(osfui::browser::kMaxMessageBytes, 'x')) == Pipe::WriteResult::Written;
	});
	std::this_thread::sleep_for(25ms);
	std::thread queuedWriter([&] {
		queuedWrote = server.WriteMessage("queued") == Pipe::WriteResult::Written;
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
	CHECK(client.WriteMessage("reopened") == Pipe::WriteResult::Written);
	payload.clear();
	CHECK(server.ReadMessage(payload));
	CHECK(payload == "reopened");

	// Concurrent writers never split a header from its payload: every frame arrives whole.
	{
		constexpr int kWriters = 4;
		constexpr int kPerWriter = 50;
		std::atomic_int written{ 0 };
		std::vector<std::thread> writers;
		for (int w = 0; w < kWriters; ++w) {
			writers.emplace_back([&, w] {
				for (int i = 0; i < kPerWriter; ++i) {
					const auto size = static_cast<std::size_t>((w * 7919 + i * 104729) % 200000 + 1);
					if (client.WriteMessage(std::string(size, static_cast<char>('a' + w))) == Pipe::WriteResult::Written) ++written;
				}
			});
		}
		int intact = 0;
		for (int n = 0; n < kWriters * kPerWriter; ++n) {
			payload.clear();
			if (!server.ReadMessage(payload, 5000)) break;
			if (payload.find_first_not_of(payload.front()) == std::string::npos) ++intact;
		}
		for (auto& thread : writers) thread.join();
		CHECK(written == kWriters * kPerWriter);
		CHECK(intact == kWriters * kPerWriter);
	}
	server.Close();
	client.Close();

	// The caller's handles are reclaimed on setup failure or immediate rejection.
	{
		std::vector<HANDLE> handles;
		{ auto abandoned = MakeHandleBatch(handles); }
		CheckHandles(handles, false);
		Pipe host;
		PipeWriter outbound(host, [] {});
		for (const auto& message : { std::string("closed"), std::string{},
			std::string(osfui::browser::kMaxMessageBytes + 1, 'x') }) {
			handles.clear();
			const auto result = outbound.Enqueue(message, MakeHandleBatch(handles));
			CHECK(result == (message == "closed" ? PipeWriter::Result::Closed : PipeWriter::Result::InvalidPayload));
			CheckHandles(handles, false);
		}
	}

	// A silent game cannot block producers. All messages, including frames across a
	// presentation change, arrive in FIFO order once it resumes reading.
	{
		Pipe game, host;
		CHECK(ConnectPair(game, host));
		std::atomic_int failures{};
		PipeWriter outbound(host, [&] { ++failures; });
		CHECK(outbound.Enqueue("before start") == PipeWriter::Result::Closed);
		outbound.Start();
		CHECK(outbound.Enqueue("") == PipeWriter::Result::InvalidPayload);
		CHECK(outbound.Enqueue(std::string(osfui::browser::kMaxMessageBytes + 1, 'x')) == PipeWriter::Result::InvalidPayload);
		const std::vector<std::string> messages{
			"hello", std::string(osfui::browser::kMaxMessageBytes, 'x'),
			"textures:generation1", "frame:slot0:serial1:epoch1", "log",
			"frame:slot1:serial2:epoch2", "frame:slot2:serial3:epoch2", "bye"
		};
		const auto started = std::chrono::steady_clock::now();
		for (const auto& message : messages) CHECK(outbound.Enqueue(message) == PipeWriter::Result::Queued);
		CHECK(std::chrono::steady_clock::now() - started < 1s);
		for (const auto& expected : messages) {
			std::string actual;
			CHECK(game.ReadMessage(actual, 2000));
			CHECK(actual == expected);
		}
		outbound.Stop(250ms);
		CHECK(failures == 0);
		CHECK(outbound.Failed() == PipeWriter::Failure::None);
		CHECK(outbound.Enqueue("after stop") == PipeWriter::Result::Closed);
	}

	// Shutdown drains queued messages before closing when the peer is healthy.
	{
		Pipe game, host;
		CHECK(ConnectPair(game, host));
		std::atomic_int failures{};
		PipeWriter outbound(host, [&] { ++failures; });
		outbound.Start();
		std::vector<std::string> received;
		std::thread consumer([&] {
			std::string message;
			while (game.ReadMessage(message, 2000)) received.push_back(message);
		});
		std::vector<HANDLE> handles;
		CHECK(outbound.Enqueue("textures", MakeHandleBatch(handles)) == PipeWriter::Result::Queued);
		CHECK(outbound.Enqueue("frame") == PipeWriter::Result::Queued);
		CHECK(outbound.Enqueue("bye") == PipeWriter::Result::Queued);
		outbound.Stop(250ms);
		consumer.join();
		CHECK((received == std::vector<std::string>{ "textures", "frame", "bye" }));
		CHECK(failures == 0);
		outbound.Stop(); // Repeated teardown cannot reclaim handles already delivered.
		CheckHandles(handles, true);
		for (const auto handle : handles) CHECK(::CloseHandle(handle));
	}

	// A blocked writer and reader are both cancelled before shutdown joins them.
	{
		Pipe game, host;
		CHECK(ConnectPair(game, host));
		std::atomic_int failures{};
		PipeWriter outbound(host, [&] { ++failures; });
		outbound.Start();
		bool readerCompleted = true;
		std::thread consumer([&] { std::string ignored; readerCompleted = host.ReadMessage(ignored); });
		std::vector<HANDLE> handles;
		CHECK(outbound.Enqueue(std::string(osfui::browser::kMaxMessageBytes, 'x'), MakeHandleBatch(handles)) == PipeWriter::Result::Queued);
		CHECK(outbound.Enqueue("queued textures", MakeHandleBatch(handles)) == PipeWriter::Result::Queued);
		std::this_thread::sleep_for(25ms);
		const auto started = std::chrono::steady_clock::now();
		outbound.Stop(25ms);
		consumer.join();
		CHECK(std::chrono::steady_clock::now() - started < 2s);
		CHECK(!readerCompleted);
		CHECK(failures == 0); // Intentional cancellation is not a transport failure.
		// Covers the cancelled in-flight announcement and the never-written queue entry.
		CheckHandles(handles, false);
	}

	// Overflow is terminal: never skip a frame announcement and continue reusing slots.
	{
		Pipe game, host;
		CHECK(ConnectPair(game, host));
		std::atomic_int failures{};
		PipeWriter outbound(host, [&] { ++failures; }, 2, 4u * osfui::browser::kMaxMessageBytes);
		outbound.Start();
		auto result = PipeWriter::Result::Queued;
		std::vector<HANDLE> handles;
		for (int i = 0; i < 4 && result == PipeWriter::Result::Queued; ++i) {
			result = outbound.Enqueue(std::string(osfui::browser::kMaxMessageBytes, 'x'), MakeHandleBatch(handles));
		}
		CHECK(result == PipeWriter::Result::Full);
		CHECK(outbound.Failed() == PipeWriter::Failure::Overflow);
		CHECK(failures == 1);
		CHECK(outbound.Enqueue("later frame") == PipeWriter::Result::Closed);
		outbound.Stop();
		CHECK(failures == 1);
		CheckHandles(handles, false);
	}
	{
		Pipe game, host;
		CHECK(ConnectPair(game, host));
		std::atomic_int failures{};
		PipeWriter outbound(host, [&] { ++failures; }, 100, 64);
		outbound.Start();
		std::vector<HANDLE> handles;
		CHECK(outbound.Enqueue(std::string(65, 'x'), MakeHandleBatch(handles)) == PipeWriter::Result::Full);
		CheckHandles(handles, false);
		CHECK(outbound.Failed() == PipeWriter::Failure::Overflow);
		CHECK(failures == 1);
		CHECK(outbound.Enqueue("later frame") == PipeWriter::Result::Closed);
	}

	// A write failure wakes the owner once; producers cannot append after failure.
	{
		Pipe game, host;
		CHECK(ConnectPair(game, host));
		const HANDLE failed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		std::atomic_int failures{};
		PipeWriter outbound(host, [&] { ++failures; ::SetEvent(failed); });
		outbound.Start();
		game.Close();
		std::vector<HANDLE> handles;
		CHECK(outbound.Enqueue("textures", MakeHandleBatch(handles)) == PipeWriter::Result::Queued);
		CHECK(::WaitForSingleObject(failed, 2000) == WAIT_OBJECT_0);
		CHECK(outbound.Failed() == PipeWriter::Failure::Write);
		CHECK(outbound.Enqueue("later frame") == PipeWriter::Result::Closed);
		outbound.Stop();
		CHECK(failures == 1);
		CheckHandles(handles, false);
		::CloseHandle(failed);
	}

	::SetEvent(watchdogDone);
	std::fprintf(stderr, "wv2_pipe_tests: %d checks, %d failure(s)\n",
		g_checks, g_failures);
	return g_failures;
}
