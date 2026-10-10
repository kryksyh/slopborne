// SPDX-License-Identifier: GPL-2.0-or-later
// Real x86/Rosetta faults: a bad descriptor must not poison the shared walker.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <xbyak/xbyak.h>
#include "bbport_toggles.h"
#include "core/signals.h"
#include "shader_recompiler/ir/passes/srt.h"

static std::atomic<unsigned> faults{};
static std::atomic<unsigned> fallbacks{};

static void OnFault(int sig, siginfo_t* info, void* context) {
    if (Core::Signals::Instance()->DispatchAccessViolation(context, info->si_addr)) {
        ++faults;
        return;
    }
    if (runtime_fault_recover) {
        auto* recover = runtime_fault_recover;
        runtime_fault_recover = nullptr;
        ++fallbacks;
        siglongjmp(*recover, 1);
    }
    _exit(128 + sig);
}

static void Check(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

int main() {
    struct sigaction handler{};
    handler.sa_sigaction = OnFault;
    handler.sa_flags = SA_SIGINFO;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGSEGV, &handler, nullptr);
    sigaction(SIGBUS, &handler, nullptr);

    // Production deserialization entry point registers the same handler as shader compilation.
    Xbyak::CodeGenerator code;
    using namespace Xbyak::util;
    code.mov(rdi, code.ptr[rdi]);
    code.mov(r10d, code.dword[rdi]);
    code.mov(code.dword[rsi], r10d);
    code.ret();
    auto walker = Shader::RegisterWalkerCode(code.getCode(), code.getSize());
    const auto* bytes = reinterpret_cast<const unsigned char*>(walker);
    const std::vector<unsigned char> before(bytes, bytes + code.getSize());
    const size_t page_size = size_t(sysconf(_SC_PAGESIZE));
    void* inaccessible = mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    Check(inaccessible != MAP_FAILED, "guard page allocation");
    const u64 bad = reinterpret_cast<u64>(inaccessible);
    const u32 expected = 0xa53c72e1;
    const u64 good = reinterpret_cast<u64>(&expected);
    u32 output{};
    walker(reinterpret_cast<const u32*>(&good), &output);
    Check(output == expected, "initial valid read");
    walker(reinterpret_cast<const u32*>(&bad), &output);
    Check(output == 0, "faulting r10d read is zero");
    walker(reinterpret_cast<const u32*>(inaccessible), &output);
    Check(output == 0, "faulting pointer and value reads are zero");
    walker(reinterpret_cast<const u32*>(&good), &output);
    Check(output == expected, "valid read after a fault is preserved");

    sigjmp_buf recover;
    if (sigsetjmp(recover, 1) == 0) {
        runtime_fault_recover = &recover;
        walker(reinterpret_cast<const u32*>(&bad), &output);
        Check(false, "speculative bad read must return to its recovery point");
    }
    Check(fallbacks == 1 && runtime_fault_recover == nullptr, "speculative fallback");
    walker(reinterpret_cast<const u32*>(&good), &output);
    Check(output == expected, "valid read after speculative fallback");

    std::atomic<bool> failed{};
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 4; ++thread) {
        workers.emplace_back([&] {
            for (unsigned i = 0; i < 1000; ++i) {
                u32 value{};
                walker(reinterpret_cast<const u32*>(&bad), &value);
                if (value != 0) failed = true;
                walker(reinterpret_cast<const u32*>(&good), &value);
                if (value != expected) failed = true;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    Check(!failed, "concurrent valid/invalid reads");
    Check(std::memcmp(bytes, before.data(), before.size()) == 0, "executable code is unchanged");
    Check(faults == 4003, "all expected faults were handled");
    std::printf("PASS: %u recovered loads, %u speculative fallbacks; valid reads and shared JIT bytes preserved\n",
                faults.load(), fallbacks.load());
}
