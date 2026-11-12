// Sleeping Dogs (X360 retail) for Switch: game data checksums (sdretail_verify_data = true).
//
// WHY
//   On the console the game asks the Scaleform pool for 0x41FFF928 bytes right after loading the first UI movie
//   (gfx_file 0x1EAB0 / new_gfx_file 0x1EA58), where the PC asks for 0x10018. The reads have the same sizes on both,
//   but a damaged byte in the copy on the SD card would give exactly such a deterministic garbage size. The card
//   cannot always be put into a PC, so the console hashes the files itself.
//
// WHAT
//   Every file under game_root: "<path> <size> <hash>" to verify_data.txt next to game_root (on the console
//   sdmc:/switch/sdretail/verify_data.txt), to stderr and to the log (if it is already open).
//   The hash is FNV-1a over 64-bit little-endian words (+ the tail bytes): simple, and the desktop build computes
//   the same for a folder on the PC (run it with --sdretail_verify_data true --game_data_root <folder>).

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(sdretail_verify_data, false, "Sleeping Dogs",
                    "At startup, write the size and checksum of every game data file to the log (slow: reads "
                    "all of game_root once)");

void SdnxVerifyData(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> files;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  const auto start = std::chrono::steady_clock::now();
  std::vector<unsigned char> buffer(4u << 20);
  std::FILE* out = std::fopen((root.parent_path() / "verify_data.txt").string().c_str(), "w");
  uint64_t total = 0;
  for (const fs::path& path : files) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "[verify] %s CANNOT OPEN\n", path.lexically_relative(root).generic_string().c_str());
      continue;
    }
    uint64_t hash = 0xCBF29CE484222325ull, size = 0;
    size_t n;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
      size_t i = 0;
      for (; i + 8 <= n; i += 8) {
        uint64_t word;
        std::memcpy(&word, buffer.data() + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
      }
      for (; i < n; ++i) hash = (hash ^ buffer[i]) * 0x100000001B3ull;
      size += n;
    }
    std::fclose(f);
    total += size;
    const std::string rel = path.lexically_relative(root).generic_string();
    std::fprintf(stderr, "[verify] %s %llu %016llX\n", rel.c_str(), (unsigned long long)size,
                 (unsigned long long)hash);
    REXLOG_INFO("[verify] {} {} {:016X}", rel, size, hash);
    if (out) {
      std::fprintf(out, "%s %llu %016llX\n", rel.c_str(), (unsigned long long)size, (unsigned long long)hash);
      std::fflush(out);
    }
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::fprintf(stderr, "[verify] done: %zu files, %llu MB in %.1f s\n", files.size(),
               (unsigned long long)(total >> 20), seconds);
  REXLOG_INFO("[verify] done: {} files, {} MB in {:.1f} s", files.size(), total >> 20, seconds);
  if (out) {
    std::fprintf(out, "done: %zu files, %llu MB in %.1f s\n", files.size(), (unsigned long long)(total >> 20),
                 seconds);
    std::fclose(out);
  }
}
