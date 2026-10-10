// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <latch>
#include <thread>
#include "core/libraries/kernel/threads.h"

extern "C" void runtime_thread_attach_host(const char*) {}

int main() {
    using Libraries::Kernel::Thread;
    using namespace std::chrono_literals;

    // A worker's own Join must not detach it from its owner's lifetime.
    {
        Thread worker;
        std::latch self_joined{1}, release{1};
        worker.Run([&](std::stop_token) {
            worker.Join();
            self_joined.count_down();
            release.wait();
        });
        self_joined.wait();
        assert(worker.Joinable());
        release.count_down();
        worker.Join();
        assert(!worker.Joinable());
    }

    // Movie EOF joins a decoder while the game's Stop requests cancellation
    // and joins the same decoder. The decoder also calls Join before returning.
    for (int attempt = 0; attempt < 100; ++attempt) {
        Thread decoder;
        std::atomic<bool> exited{};
        std::latch started{1}, joining{2};
        decoder.Run([&](std::stop_token stop) {
            started.count_down();
            while (!stop.stop_requested()) std::this_thread::yield();
            decoder.Join();
            exited = true;
        });
        started.wait();
        auto join = [&] {
            joining.count_down();
            decoder.Join();
            assert(exited.load());
        };
        std::thread eof(join), other(join);
        joining.wait();
        std::this_thread::sleep_for(1ms);
        decoder.Stop();
        eof.join();
        other.join();
        assert(exited.load() && !decoder.Joinable());

        // Reuse after natural completion, including a worker that never calls Join.
        decoder.Run([](std::stop_token) {});
        while (decoder.Joinable()) std::this_thread::yield();
        decoder.Run([](std::stop_token) {});
        decoder.Join();
    }
    std::puts("PASS: self-join, concurrent movie EOF/Stop, and thread reuse");
}
