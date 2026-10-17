#pragma once
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

namespace osfui::cefhost
{
    // Arms only when dirty output has a future deadline. No periodic ticks,
    // global timer-resolution changes, spinning, or graphics on this thread.
    class FrameTimer
    {
    public:
        ~FrameTimer()
        {
            Stop();
            if (timer_) CloseHandle(timer_);
            if (stop_) CloseHandle(stop_);
        }
        void Start(std::function<void()> wake)
        {
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer_) throw std::runtime_error("Cannot create high-resolution CEF frame timer");
            stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!stop_) throw std::runtime_error("Cannot create CEF frame timer stop event");
            thread_ = std::thread([this, wake = std::move(wake)] {
                const HANDLE handles[]{ stop_, timer_ };
                for (;;) {
                    const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                    if (result == WAIT_OBJECT_0) return;
                    if (result != WAIT_OBJECT_0 + 1) { TerminateProcess(GetCurrentProcess(), 14); return; }
                    wake();
                }
            });
        }
        bool Arm(std::chrono::steady_clock::duration remaining) noexcept
        {
            using Tick = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
            LARGE_INTEGER due{};
            due.QuadPart = -(std::max)(1ll, std::chrono::ceil<Tick>(remaining).count());
            return SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
        }
        void Stop()
        {
            if (stop_) SetEvent(stop_);
            if (thread_.joinable()) thread_.join();
        }
    private:
        HANDLE timer_{}, stop_{};
        std::thread thread_;
    };
}
