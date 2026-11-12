// Sleeping Dogs (X360 retail) for Switch: memory pool allocation trace.
//
// WHY
//   First runs on the console (2026-11-11) died about 8 s in: the game's assert "UIGfxAllocator::Alloc" /
//   "Scaleform_MemoryPool" (two tw traps), then a write through the null block. The kernel gives the game exactly
//   the same memory as on PC (1096 MmAllocatePhysicalMemoryEx calls, same sizes) and the game reads the same files
//   (416 reads, same sizes), so the difference is in how the recompiled code runs on ARM. Logging the pool requests
//   on both platforms shows where they first differ.
//
// WHAT
//   sub_822684B8 = MemoryPool::Allocate(r3 pool, r4 size, r5 name, r6 flags = align << 8 | 1, r7); the UI's
//   UIGfxAllocator::Alloc (sub_8249B170) calls it with the Scaleform pool 0x82FC7858. It returns 0 when the pool has
//   no room. Logged:
//     * always: every failed request (pool, size, name, request number of that pool);
//     * with sdretail_alloc_log = true: every request of at least 64 KB, and every 2000th request of each pool
//       with the running count and bytes (cheap enough to compare a whole boot).

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long frames_to_skip,
                                                                                  unsigned long frames_to_capture,
                                                                                  void** back_trace,
                                                                                  unsigned long* back_trace_hash);
#else
#include <unwind.h>
#endif

void sdretail_RegisterFunctions(rex::runtime::IModuleRegistrar* registrar);

REXCVAR_DEFINE_BOOL(sdretail_alloc_log, false, "Sleeping Dogs",
                    "Log large memory pool requests and running totals (compare runs on PC and console)");

REXCVAR_DEFINE_BOOL(sdretail_alloc_stack, false, "Sleeping Dogs",
                    "With sdretail_alloc_log: add the guest call stack to the first 200 Scaleform pool requests");

REXCVAR_DEFINE_BOOL(sdretail_alloc_dump, false, "Sleeping Dogs",
                    "Write the Scaleform and UI pool memory to scaleform_dump.bin at the 11th Scaleform pool request "
                    "after new_gfx_file (compare PC and console)");

REX_EXTERN(__imp__sub_822684B8);  // MemoryPool::Allocate(pool, size, name, flags, r7)
REX_EXTERN(__imp__sub_823B3538);  // Scaleform state bag: set(r3 bag, r5 state), keyed by the u32 at state+8
REX_EXTERN(__imp__sub_823A4750);  // Scaleform hash set: setRawCapacity(r3 set, r5 new size)

namespace {

struct PoolStats {
  uint64_t requests = 0, bytes = 0;
};

std::string GuestString(uint8_t* base, uint32_t addr) {
  if (!addr) return "-";
  const char* s = reinterpret_cast<const char*>(base + addr);
  return std::string(s, strnlen(s, 64));
}

// Guest memory snapshot (big-endian, as the guest sees it): the Scaleform pool block and the UI pool block, at the
// same addresses on PC and console (the game allocates deterministically). File: <exe folder>/scaleform_dump.bin,
// each range as "SDNXDUMP" <start u32> <length u32> (little endian) + bytes.
void DumpPools(uint8_t* base) {
  static const uint32_t kRanges[][2] = {{0xBD9C8000u, 0x44F000u}, {0xB77E1000u, 0x259000u}};
  const std::string path = (rex::filesystem::GetExecutableFolder() / "scaleform_dump.bin").string();
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("[alloc] dump: cannot write {}", path);
    return;
  }
  for (const auto& range : kRanges) {
    std::fwrite("SDNXDUMP", 1, 8, f);
    std::fwrite(&range[0], 4, 1, f);
    std::fwrite(&range[1], 4, 1, f);
    std::fwrite(base + range[0], 1, range[1], f);
  }
  std::fclose(f);
  REXLOG_INFO("[alloc] dump written: {}", path);
  std::fprintf(stderr, "[alloc] dump written: %s\n", path.c_str());
}

// Guest call stack from the host stack: every recompiled function sub_XXXXXXXX is a host function, so each host
// return address is mapped to the registered function that starts at or below it (the table comes from the same
// registration function the runtime uses). Tail calls (b) leave no frame, and frames outside the game code (SDK,
// dispatcher) are dropped, so this is the chain of guest functions that called with bl.
class FunctionTable final : public rex::runtime::IModuleRegistrar {
 public:
  bool SetFunction(uint32_t guest_address, ::PPCFunc* func) override {
    entries_.emplace_back(reinterpret_cast<uintptr_t>(func), guest_address);
    return true;
  }
  void Build() {
    sdretail_RegisterFunctions(this);
    std::sort(entries_.begin(), entries_.end());
  }
  uint32_t Find(uintptr_t host) const {
    if (entries_.empty() || host < entries_.front().first || host > entries_.back().first + 0x40000) return 0;
    auto it = std::upper_bound(entries_.begin(), entries_.end(), std::make_pair(host, UINT32_MAX));
    return it == entries_.begin() ? 0 : std::prev(it)->second;
  }

 private:
  std::vector<std::pair<uintptr_t, uint32_t>> entries_;
};

#ifndef _WIN32
struct Backtrace {
  void** frames;
  int count, max;
};
_Unwind_Reason_Code BacktraceStep(_Unwind_Context* context, void* arg) {
  auto* bt = static_cast<Backtrace*>(arg);
  if (bt->count >= bt->max) return _URC_END_OF_STACK;
  bt->frames[bt->count++] = reinterpret_cast<void*>(_Unwind_GetIP(context));
  return _URC_NO_REASON;
}
#endif

std::string GuestStack() {
  static FunctionTable table;
  static bool built = false;
  if (!built) {
    table.Build();
    built = true;
  }
  void* frames[48];
  int count;
#ifdef _WIN32
  count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
#else
  Backtrace bt{frames, 0, 48};
  _Unwind_Backtrace(BacktraceStep, &bt);
  count = bt.count;
#endif
  std::string out;
  uint32_t last = 0;
  int printed = 0;
  for (int i = 0; i < count && printed < 24; ++i) {
    // Return addresses point after the call: -1 keeps a call at the very end of a function inside it.
    uint32_t guest = table.Find(reinterpret_cast<uintptr_t>(frames[i]) - 1);
    if (!guest || guest == last) continue;
    char text[12];
    std::snprintf(text, sizeof(text), " %08X", guest);
    out += text;
    last = guest;
    ++printed;
  }
  return out;
}

uint32_t GuestU32(uint8_t* base, uint32_t addr) {
  uint32_t value;
  std::memcpy(&value, base + addr, 4);
  return __builtin_bswap32(value);
}

}  // namespace

// The console's first extra Scaleform request (#130, 0x318) is this hash set growing to 64 entries inside the
// state bag set called from the loader constructor sub_823E26B0; on the PC it never grows that far. Logged with
// sdretail_alloc_log: which states are set (key, vtable) and every capacity change, to compare with the PC.
REX_HOOK_RAW(sub_823B3538) {
  static int calls = 0;
  const uint32_t bag = ctx.r3.u32, state = ctx.r5.u32;
  if (REXCVAR_GET(sdretail_alloc_log) && ++calls <= 300) {
    REXLOG_INFO("[state] set #{} bag {:08X} state {:08X} vtable {:08X} key {:08X}", calls, bag, state,
                state ? GuestU32(base, state) : 0, state ? GuestU32(base, state + 8) : 0);
  }
  __imp__sub_823B3538(ctx, base);
}

REX_HOOK_RAW(sub_823A4750) {
  static int calls = 0;
  const uint32_t set = ctx.r3.u32, size = ctx.r5.u32;
  const uint32_t table = GuestU32(base, set);
  const uint32_t count = table ? GuestU32(base, table) : 0, mask = table ? GuestU32(base, table + 4) : 0;
  __imp__sub_823A4750(ctx, base);
  if (REXCVAR_GET(sdretail_alloc_log) && ++calls <= 300) {
    const uint32_t new_table = GuestU32(base, set);
    REXLOG_INFO("[hash] capacity #{} set {:08X} size {} (entries {} mask {:#x}) -> mask {:#x}", calls, set, size,
                count, mask, new_table ? GuestU32(base, new_table + 4) : 0);
  }
}

REX_HOOK_RAW(sub_822684B8) {
  const uint32_t pool = ctx.r3.u32, size = ctx.r4.u32, name = ctx.r5.u32, flags = ctx.r6.u32;
  if (pool == 0x82FC7858u && REXCVAR_GET(sdretail_alloc_dump)) {
    // Counted before the call: the dump shows memory as it is when the request is made.
    static int after_new_gfx = -1;
    if (after_new_gfx >= 0 && ++after_new_gfx == 11) {
      REXLOG_INFO("[alloc] dump at Scaleform request size {:#x}", size);
      DumpPools(base);
    }
    if (after_new_gfx < 0 && name && std::strncmp(reinterpret_cast<const char*>(base + name), "new_gfx_file", 12) == 0) {
      after_new_gfx = 0;
    }
  }
  __imp__sub_822684B8(ctx, base);
  const uint32_t result = ctx.r3.u32;

  static std::mutex mutex;
  static std::unordered_map<uint32_t, PoolStats> pools;
  const bool verbose = REXCVAR_GET(sdretail_alloc_log);
  if (result && !verbose) return;
  std::lock_guard<std::mutex> lock(mutex);
  PoolStats& stats = pools[pool];
  ++stats.requests;
  stats.bytes += size;
  if (!result) {
    REXLOG_ERROR("[alloc] FAILED: pool {:08X} size {:#x} ({} KB) flags {:#x} name '{}' (request {} of this pool, "
                 "{} KB requested so far)",
                 pool, size, size >> 10, flags, GuestString(base, name), stats.requests, stats.bytes >> 10);
    return;
  }
  if (pool == 0x82FC7858u && stats.requests <= 400) {
    // Every Scaleform pool request of the first 400: the PC and the console diverge in this sequence.
    const std::string stack = REXCVAR_GET(sdretail_alloc_stack) && stats.requests <= 200 ? GuestStack() : "";
    REXLOG_INFO("[alloc] sf #{} size {:#x} flags {:#x} name '{}' -> {:08X}{}{}", stats.requests, size, flags,
                GuestString(base, name), result, stack.empty() ? "" : " stack", stack);
    return;
  }
  if (size >= 0x10000) {
    REXLOG_INFO("[alloc] pool {:08X} #{} size {:#x} name '{}' -> {:08X}", pool, stats.requests, size,
                GuestString(base, name), result);
  }
  if (stats.requests % 2000 == 0) {
    REXLOG_INFO("[alloc] pool {:08X}: {} requests, {} KB", pool, stats.requests, stats.bytes >> 10);
  }
}
