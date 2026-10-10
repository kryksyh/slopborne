// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Common {

void* GetXmmPointer(void* ctx, u8 index);

void* GetRip(void* ctx);

void IncrementRip(void* ctx, u64 length);

// x86 encoding order: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15.
// Returns null for unsupported architectures or an invalid register index.
u64* GetX64GprPointer(void* ctx, u8 index);

bool IsWriteError(void* ctx);

} // namespace Common
