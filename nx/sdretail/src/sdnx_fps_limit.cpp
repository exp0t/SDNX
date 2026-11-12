// Sleeping Dogs (X360 retail) for Switch: frame-rate limit 30 / 60 / off, with a row in Settings > Display.
//
// WHY THE GAME RUNS AT 30
//   The game's Direct3D queues every frame flip with a presentation interval (sub_82BBC580, r3 bits 8-11): the flip
//   happens at last-flip vblank + interval. The game uses 2 on the 60 Hz vblank the SDK emulates, so it never goes
//   above 30 fps, however fast the console is. Lowering the interval lifts that cap; nothing else in the game is
//   tied to it (the engine times its simulation with the clock, as on PC).
//
// WHAT THIS DOES
//   * sub_82BBC580 (flip queue): interval left as the game sets it for 30, at most 1 for 60, 0 for "off".
//   * sub_827ECC70 (UIScreenOptionsDisplay::handleMessage): the spinner row added to the Options_Display movie
//     (port/tools/fps_menu_patch.py, applied by apply_branding.py) sends FSCommand "OPTIONS_FPS_LIMITER" with the
//     index 0/1/2; the message id is qStringHashUpper32("OPTIONS_FPS_LIMITER") = 0x3EAA29C9. The value is read
//     with the game's own sub_827AD340(msg), the cvar is set and saved to sdretail.toml.
//   * sub_827C9358 (the screen's "set values" step: Invoke OptionList_SetValues + OptionList_PlayIntro): afterwards
//     Invoke("SDNX_SetFpsLimit", index) on the screen's movie, so the row shows the current value. Movie =
//     *(*(screen + 76) + 312), Invoke = movie vtable +92 (r3 movie, r4 name, r5 result, r6 GFxValue[], r7 count),
//     GFxValue number = sub_8270E9B0(r3 value*, f1) — the same calls the game makes in this function.
//   Without the patched movie the hooks only see the cvar (sdretail_fps_limit in sdretail.toml), and the Invoke of
//   an unknown AS function is ignored by GFx.

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

REXCVAR_DEFINE_INT32(sdretail_fps_limit, 30, "Sleeping Dogs",
                     "Frame rate limit: 30 (the game's own), 60, or 0 = no limit. Also in Settings > Display");
REXCVAR_DEFINE_BOOL(sdretail_fps_log, false, "Sleeping Dogs",
                    "Log the measured frame-flip rate every 5 seconds (checks the limit without a screen capture)");

REX_EXTERN(__imp__sub_82BBC580);  // D3D: queue a frame flip (r3 = front buffer | interval << 8 | threshold)
REX_EXTERN(__imp__sub_827ECC70);  // UIScreenOptionsDisplay::handleMessage(this, msg id, msg)
REX_EXTERN(__imp__sub_827C9358);  // UIScreenOptionsDisplay: push the option values to the movie
REX_EXTERN(sub_827AD340);         // UI message -> integer value of its argument
REX_EXTERN(sub_8270E9B0);         // GFxValue::SetNumber(this, f1)

namespace {

constexpr uint32_t kFpsLimiterMessage = 0x3EAA29C9;  // qStringHashUpper32("OPTIONS_FPS_LIMITER")
constexpr char kSetFunction[] = "SDNX_SetFpsLimit";

int Normalized(int limit) { return limit == 60 || limit == 0 ? limit : 30; }
int IndexOf(int limit) { return limit == 60 ? 1 : limit == 0 ? 2 : 0; }
int LimitOf(int index) { return index == 1 ? 60 : index == 2 ? 0 : 30; }

uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

// Writes `sdretail_fps_limit = N` into sdretail.toml next to the executable, keeping every other line (comments,
// [log.levels], other settings). rex::cvar::SaveConfig is not used: it rewrites the file with the cvars only.
void SaveLimit(int limit) {
  REXCVAR_SET(sdretail_fps_limit, limit);
  const std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "sdretail.toml";
  const std::string setting = "sdretail_fps_limit = " + std::to_string(limit);

  std::vector<std::string> lines;
  if (std::ifstream in(path); in) {
    for (std::string l; std::getline(in, l);) lines.push_back(l);
  }
  bool done = false;
  size_t first_table = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    const size_t p = lines[i].find_first_not_of(" \t");
    if (p == std::string::npos) continue;
    if (lines[i][p] == '[') {
      first_table = std::min(first_table, i);
      continue;
    }
    if (i < first_table && lines[i].compare(p, 18, "sdretail_fps_limit") == 0) {
      lines[i] = setting;
      done = true;
    }
  }
  if (!done) lines.insert(lines.begin() + std::ptrdiff_t(first_table), setting);  // root keys go before tables
  if (std::ofstream out(path, std::ios::trunc); out) {
    for (const auto& l : lines) out << l << '\n';
  } else {
    REXLOG_WARN("[fps_limit] cannot write {}", path.string());
  }
  REXLOG_INFO("[fps_limit] set to {} from the Display menu", limit ? std::to_string(limit) : std::string("off"));
}

}  // namespace

REX_HOOK_RAW(sub_82BBC580) {
  const uint32_t r3 = ctx.r3.u32;
  const uint32_t game_interval = (r3 >> 8) & 0xF;
  const int limit = Normalized(REXCVAR_GET(sdretail_fps_limit));
  uint32_t interval = game_interval;
  if (limit == 60 && interval > 1) interval = 1;
  if (limit == 0) interval = 0;
  static std::atomic<uint32_t> last_logged{0xFFFFFFFFu};
  const uint32_t key = (game_interval << 4) | interval;
  if (last_logged.exchange(key) != key) {
    REXLOG_INFO("[fps_limit] presentation interval: game {}, used {} (limit {})", game_interval, interval, limit);
  }
  ctx.r3.u64 = (r3 & ~0xF00u) | (interval << 8);
  __imp__sub_82BBC580(ctx, base);

  if (REXCVAR_GET(sdretail_fps_log)) {
    using clock = std::chrono::steady_clock;
    static std::atomic<uint32_t> flips{0};
    static std::atomic<int64_t> window_start{0};
    const int64_t now = clock::now().time_since_epoch().count();
    int64_t start = window_start.load();
    if (start == 0) window_start.compare_exchange_strong(start, now);
    const uint32_t n = flips.fetch_add(1) + 1;
    const double seconds = double(now - window_start.load()) * clock::period::num / clock::period::den;
    if (seconds >= 5.0) {
      REXLOG_INFO("[fps_limit] {:.1f} flips/s over {:.1f} s (limit {}, interval {})", n / seconds, seconds, limit,
                  interval);
      flips.store(0);
      window_start.store(now);
    }
  }
}

REX_HOOK_RAW(sub_827ECC70) {
  if (ctx.r4.u32 != kFpsLimiterMessage) {
    __imp__sub_827ECC70(ctx, base);
    return;
  }
  PPCContext saved = ctx;
  ctx.r3.u64 = saved.r5.u64;
  sub_827AD340(ctx, base);
  const int index = ctx.r3.s32;
  ctx = saved;
  SaveLimit(LimitOf(index));
  ctx.r3.u64 = 1;  // handled
}

REX_HOOK_RAW(sub_827C9358) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_827C9358(ctx, base);

  const uint32_t holder = screen ? Load32(base, screen + 76) : 0;
  const uint32_t movie = holder ? Load32(base, holder + 312) : 0;
  if (!movie) return;

  PPCContext saved = ctx;
  // Scratch area in a new guest stack frame: callees build their frames below r1, so r1 + 0x40.. stays ours.
  ctx.r1.u64 = saved.r1.u32 - 0x100;
  const uint32_t frame = ctx.r1.u32;
  const uint32_t value = frame + 0x40;  // GFxValue (12 bytes)
  const uint32_t name = frame + 0x60;
  std::memset(base + value, 0, 16);
  std::memcpy(base + name, kSetFunction, sizeof(kSetFunction));

  ctx.r3.u64 = value;
  ctx.f1.f64 = double(IndexOf(Normalized(REXCVAR_GET(sdretail_fps_limit))));
  sub_8270E9B0(ctx, base);

  const uint32_t invoke = Load32(base, Load32(base, movie) + 92);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = movie;
  ctx.r4.u64 = name;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = value;
  ctx.r7.u64 = 1;
  bool shown = false;
  if (PPCFunc* fn = rex::runtime::ResolveIndirectFunction(invoke)) {
    fn(ctx, base);
    shown = (ctx.r3.u32 & 0xFF) != 0;
  }
  REXLOG_INFO("[fps_limit] Display menu opened: {}(index {}) {}", kSetFunction,
              IndexOf(Normalized(REXCVAR_GET(sdretail_fps_limit))),
              shown ? "accepted by the movie" : "not found in the movie (data not patched?)");
  ctx = saved;
}
