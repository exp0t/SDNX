// Sleeping Dogs for Nintendo Switch: hooks that change only how fast the game runs, never what it does.

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#endif

REX_EXTERN(__imp__sub_82BB83F0);

namespace {
std::atomic<uint64_t> g_fence_wait_yields{0};
}

// One step of Direct3D's fence wait (BlockOnFence; called in a loop by sub_82BC01C8 until the GPU passes the fence):
// cctpl, 4 x 8 db16cyc, cctpm, then the checks; r3 = 1 means "still waiting". On Xenon the low thread priority and the
// delays hand the core to the other hardware thread; recompiled they are an empty busy loop - in gameplay the four
// game worker threads spent ~30 % of a core each here (Switch profile 2026-11-13), on three cores shared with the
// GPU command thread and the main thread that they are waiting for. The step now gives its core away while waiting.
REX_HOOK_RAW(sub_82BB83F0) {
  __imp__sub_82BB83F0(ctx, base);
  if (ctx.r3.u32 != 0) {
    g_fence_wait_yields.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
    // A short real sleep, not a bare yield: a yield returns at once when no thread of the same priority is ready on
    // this core, and the GPU thread that has to make progress has a higher priority on another core anyway.
    svcSleepThread(50'000);  // 50 us; the fence advances in steps of a frame part (milliseconds)
#else
    std::this_thread::yield();
#endif
  }
}

uint64_t SdnxFenceWaitYields() { return g_fence_wait_yields.load(std::memory_order_relaxed); }
