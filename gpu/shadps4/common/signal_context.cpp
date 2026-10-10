// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

void IncrementRip(void* ctx, u64 length) {
#if defined(_WIN32)
    ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip += length;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__rip += length;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    ((ucontext_t*)ctx)->uc_mcontext->__ss.__pc += length;
#elif defined(__FreeBSD__)
    ((ucontext_t*)ctx)->uc_mcontext.mc_rip += length;
#elif defined(ARCH_X86_64)
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP] += length;
#endif
}

u64* GetX64GprPointer(void* ctx, u8 index) {
    if (index >= 16) return nullptr;
#if defined(_WIN32) && defined(ARCH_X86_64)
    auto* s = ((EXCEPTION_POINTERS*)ctx)->ContextRecord;
    u64* regs[] = {&s->Rax, &s->Rcx, &s->Rdx, &s->Rbx, &s->Rsp, &s->Rbp, &s->Rsi, &s->Rdi,
                   &s->R8, &s->R9, &s->R10, &s->R11, &s->R12, &s->R13, &s->R14, &s->R15};
    return regs[index];
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    auto* s = &((ucontext_t*)ctx)->uc_mcontext->__ss;
    decltype(&s->__rax) regs[] = {&s->__rax, &s->__rcx, &s->__rdx, &s->__rbx, &s->__rsp, &s->__rbp,
                    &s->__rsi, &s->__rdi, &s->__r8, &s->__r9, &s->__r10, &s->__r11,
                    &s->__r12, &s->__r13, &s->__r14, &s->__r15};
    return reinterpret_cast<u64*>(regs[index]);
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    auto* s = &((ucontext_t*)ctx)->uc_mcontext;
    decltype(&s->mc_rax) regs[] = {&s->mc_rax, &s->mc_rcx, &s->mc_rdx, &s->mc_rbx, &s->mc_rsp, &s->mc_rbp,
                    &s->mc_rsi, &s->mc_rdi, &s->mc_r8, &s->mc_r9, &s->mc_r10, &s->mc_r11,
                    &s->mc_r12, &s->mc_r13, &s->mc_r14, &s->mc_r15};
    return reinterpret_cast<u64*>(regs[index]);
#elif defined(ARCH_X86_64)
    constexpr int regs[] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI,
                            REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13,
                            REG_R14, REG_R15};
    return reinterpret_cast<u64*>(&((ucontext_t*)ctx)->uc_mcontext.gregs[regs[index]]);
#else
    return nullptr;
#endif
}

} // namespace Common
