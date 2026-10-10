// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"
#include "core/libraries/kernel/threads/pthread.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        Stop(); // Reap a completed run before reusing the thread object.
        std::lock_guard lock(mutex);
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
            func(stop);
            done->store(true);
        });
        id = thread.get_id();
        stop_source = thread.get_stop_source();
    }
    // AvPlayer joins its decoders from both Stop() and the demuxer, and calls
    // Join() from inside each worker on exit. Only one external caller may touch
    // the std::jthread. A self-join leaves ownership with that caller instead of
    // detaching a thread that another caller may already be joining.
    void Join() {
        std::unique_lock lock(mutex);
        if (id == std::this_thread::get_id()) return;
        joined.wait(lock, [this] { return !joining; });
        if (!thread.joinable()) return;
        joining = true;
        lock.unlock();
        try {
            thread.join();
        } catch (const std::system_error& error) {
            // Never unwind through guest frames or release resources while the
            // worker still uses them, even if the host join implementation fails.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n",
                         error.what());
            while (!finished->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (thread.joinable()) thread.detach();
        }
        lock.lock();
        id = {};
        joining = false;
        joined.notify_all();
    }
    bool Joinable() const {
        std::lock_guard lock(mutex);
        return id != std::thread::id{} && !finished->load();
    }
    void Stop() {
        // Request cancellation without waiting for an in-progress Join(): that
        // join may itself be waiting for this cancellation to release a worker.
        std::stop_source source;
        {
            std::lock_guard lock(mutex);
            source = stop_source;
        }
        source.request_stop();
        Join();
    }

private:
    mutable std::mutex mutex;
    std::condition_variable joined;
    bool joining{};
    std::thread::id id{};
    std::stop_source stop_source;
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
