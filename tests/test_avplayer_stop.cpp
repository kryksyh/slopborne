// SPDX-License-Identifier: GPL-2.0-or-later
// Decode a supplied movie through the production AvPlayer, then stop mid-stream.
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/memory.h"

// Decoder-only harness: allocated frame buffers have no GPU cache to invalidate.
void Core::MemoryManager::InvalidateMemory(VAddr, u64) {}

using namespace Libraries::AvPlayer;

struct State final : AvPlayerStateCallback {
    std::atomic<bool> failed{};
    AvPlayerAvSyncMode GetSyncMode() override { return AvPlayerAvSyncMode::None; }
    void OnWarning(u32) override {}
    void OnError() override { failed = true; }
    void OnEOF() override {}
};

static void* PS4_SYSV_ABI Allocate(void*, u32 alignment, u32 size) {
    void* result{};
    return posix_memalign(&result, std::max<size_t>(alignment, sizeof(void*)), size) == 0
               ? result : nullptr;
}
static void PS4_SYSV_ABI Free(void*, void* buffer) { std::free(buffer); }
static s32 PS4_SYSV_ABI Open(void* object, const char* path) {
    return *static_cast<int*>(object) = open(path, O_RDONLY);
}
static s32 PS4_SYSV_ABI Close(void* object) { return close(*static_cast<int*>(object)); }
static s32 PS4_SYSV_ABI Read(void* object, u8* buffer, u64 offset, u32 size) {
    return pread(*static_cast<int*>(object), buffer, size, offset);
}
static u64 PS4_SYSV_ABI Size(void* object) {
    struct stat info{};
    assert(fstat(*static_cast<int*>(object), &info) == 0);
    return info.st_size;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace std::chrono_literals;
    for (int iteration = 0; iteration < 5; ++iteration) {
        State state;
        int fd = -1;
        AvPlayerInitData init{};
        init.memory_replacement = {nullptr, Allocate, Free, Allocate, Free};
        init.file_replacement = {&fd, Open, Close, Read, Size};
        init.num_output_video_framebuffers = 4;
        AvPlayerSource source(state);
        assert(source.Init(init, argv[1]));
        assert(source.FindStreams());
        for (int i = 0; i < source.GetStreamCount(); ++i) {
            assert(source.EnableStream(i));
        }
        assert(source.Start());
        unsigned video = 0, audio = 0;
        auto deadline = std::chrono::steady_clock::now() + 10s;
        while ((video < 12 || audio < 12) && std::chrono::steady_clock::now() < deadline) {
            AvPlayerFrameInfo frame{};
            video += source.GetVideoData(frame);
            audio += source.GetAudioData(frame);
            std::this_thread::sleep_for(1ms);
        }
        assert(!state.failed && video >= 12 && audio >= 12);
        auto before = std::chrono::steady_clock::now();
        assert(source.Stop());
        assert(std::chrono::steady_clock::now() - before < 2s);
        std::printf("PASS: movie run %d decoded %u video / %u audio frames and stopped\n",
                    iteration + 1, video, audio);
    }
}
