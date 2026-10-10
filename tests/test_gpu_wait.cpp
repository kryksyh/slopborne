// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production semaphore wait with deterministic driver results.
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL: %s\n", #value); std::abort(); } } while (0)

static unsigned calls;
static bool completed;
static bool lose_device;

static VKAPI_ATTR VkResult VKAPI_CALL Counter(VkDevice, VkSemaphore, uint64_t* value) {
    *value = completed ? 1 : 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice, const VkSemaphoreWaitInfo* info, uint64_t) {
    CHECK(info->semaphoreCount == 1 && info->pValues[0] == 1);
    ++calls;
    if (lose_device) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (calls < 3) {
        return VK_TIMEOUT;
    }
    completed = true;
    return VK_SUCCESS;
}

int main(int argc, char** argv) {
    lose_device = argc == 2 && std::strcmp(argv[1], "--device-lost") == 0;
    Vulkan::Instance instance(0, false);
    Vulkan::Semaphore semaphore(instance);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue = Counter;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkWaitSemaphores = Wait;
    semaphore.Wait(1);
    CHECK(!lose_device && completed && calls == 3 && semaphore.KnownGpuTick() == 1);
    semaphore.Wait(1);
    CHECK(calls == 3);
    std::puts("PASS: timeout retries, successful completion and already-completed fast path");
}
