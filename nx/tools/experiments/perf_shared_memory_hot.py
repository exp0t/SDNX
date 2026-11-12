#!/usr/bin/env python3
"""Perf patch for the ReXGlue / masseffect-nx SDK shared memory: stop write-watching "hot" pages.

Guest memory uploaded to the GPU copy is write-protected so a CPU write invalidates it (page fault -> unprotect ->
invalidate -> re-upload -> protect again). Sleeping Dogs rewrites its dynamic vertex / index data every frame, many
times per page (ring buffers advance through a page draw by draw): ~11 000 faults/s and ~40 000 protect calls/s;
VirtualProtect plus the global lock the fault handlers hold cost ~25-30% of the "GPU Commands" thread on the
desktop bench, and on the Switch both are Horizon svcs.

A page that faults N times (cvar sdnx_hot_page_faults, default 4) within a window (sdnx_hot_page_window_ms,
default 2000) becomes hot: it is no longer marked valid nor protected after an upload, so every request uploads
its current CPU contents again (always correct, costs copies). Hot pages are forgotten every window so pages that
calm down get cached again. sdnx_hot_pages = false restores the original behaviour.
Needs perf_shared_memory_coalesce.py applied first.

Usage: perf_shared_memory_hot.py <sdk root>      (idempotent)
"""
import sys

root = sys.argv[1].rstrip("/\\")
mark = "SDNX perf: hot"


def patch(path, edits):
    s = open(path, encoding="utf-8").read().replace("\r\n", "\n")
    if mark in s:
        print("already patched:", path)
        return
    for old, new in edits:
        if s.count(old) != 1:
            sys.exit("pattern not found (%d) in %s:\n%s" % (s.count(old), path, old))
        s = s.replace(old, new)
    open(path, "w", encoding="utf-8", newline="\n").write(s)
    print("patched:", path)


patch(root + "/include/rex/graphics/shared_memory.h", [(
    "  std::vector<uint64_t> system_page_flags_stale_;\n",
    "  std::vector<uint64_t> system_page_flags_stale_;\n"
    "  // %s pages: rewritten by the CPU often - neither cached as valid nor write-watched.\n"
    "  std::vector<uint64_t> system_page_flags_hot_;\n"
    "  std::vector<uint8_t> page_fault_count_;\n"
    "  uint64_t hot_window_start_ms_ = 0;\n" % mark,
)])

patch(root + "/src/graphics/shared_memory.cpp", [
    ("#include <rex/memory.h>\n",
     "#include <rex/memory.h>\n#include <rex/cvar.h>\n\n#include <chrono>\n\n"
     "// %s pages (see nx/tools/perf_shared_memory_hot.py in the Sleeping Dogs port).\n"
     "REXCVAR_DEFINE_BOOL(sdnx_hot_pages, true, \"GPU\",\n"
     "                    \"Do not write-watch guest pages rewritten often, re-upload them on use\");\n"
     "REXCVAR_DEFINE_INT32(sdnx_hot_page_faults, 4, \"GPU\", \"Write faults within the window that make a page hot\");\n"
     "REXCVAR_DEFINE_INT32(sdnx_hot_page_window_ms, 2000, \"GPU\", \"Hot page window, ms\");\n" % mark),
    ("  system_page_flags_stale_.assign(num_system_page_flags_, 0);  // SDNX perf: stale\n",
     "  system_page_flags_stale_.assign(num_system_page_flags_, 0);  // SDNX perf: stale\n"
     "  system_page_flags_hot_.assign(num_system_page_flags_, 0);\n"
     "  page_fault_count_.assign(size_t(num_system_page_flags_) * 64, 0);\n"),
    ("  system_page_flags_stale_.clear();\n",
     "  system_page_flags_stale_.clear();\n  system_page_flags_hot_.clear();\n  page_fault_count_.clear();\n"),
    # MakeRangeValid: hot pages stay invalid and unwatched.
    ("""  {
    auto global_lock = global_critical_region_.Acquire();

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = UINT64_MAX;""",
     """  bool skip_hot = !written_by_gpu && REXCVAR_GET(sdnx_hot_pages);
  bool any_hot = false;
  {
    auto global_lock = global_critical_region_.Acquire();

    for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
      uint64_t valid_bits = skip_hot ? ~system_page_flags_hot_[i] : UINT64_MAX;
      any_hot |= skip_hot && system_page_flags_hot_[i] != 0;"""),
    ("""  if (memory_invalidation_callback_handle_) {
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
  }
}""",
     """  if (memory_invalidation_callback_handle_) {
    if (!any_hot) {
      memory().EnablePhysicalMemoryAccessCallbacks(
          valid_page_first << page_size_log2_,
          (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
    } else {
      // SDNX perf: hot pages are not watched - watch the runs between them.
      uint32_t run_first = UINT32_MAX;
      for (uint32_t page = valid_page_first; page <= valid_page_last + 1; ++page) {
        bool hot = page > valid_page_last || ((system_page_flags_hot_[page >> 6] >> (page & 63)) & 1);
        if (!hot && run_first == UINT32_MAX) {
          run_first = page;
        } else if (hot && run_first != UINT32_MAX) {
          memory().EnablePhysicalMemoryAccessCallbacks(run_first << page_size_log2_,
                                                       (page - run_first) << page_size_log2_, true, false);
          run_first = UINT32_MAX;
        }
      }
    }
  }
}"""),
    # Invalidation: count faults of the exact pages, make them hot.
    ("""  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {""",
     """  auto global_lock = global_critical_region_.Acquire();

  if (REXCVAR_GET(sdnx_hot_pages) && !page_fault_count_.empty()) {
    // SDNX perf: hot pages - count the CPU writes to pages that were valid.
    uint32_t threshold = uint32_t(std::max(REXCVAR_GET(sdnx_hot_page_faults), int32_t(1)));
    for (uint32_t page = page_first; page <= page_last; ++page) {
      uint64_t bit = uint64_t(1) << (page & 63);
      if (!(system_page_flags_valid_[page >> 6] & bit) ||
          (system_page_flags_valid_and_gpu_written_[page >> 6] & bit)) {
        continue;
      }
      uint8_t& count = page_fault_count_[page];
      if (count < 255) {
        ++count;
      }
      if (count >= threshold) {
        system_page_flags_hot_[page >> 6] |= bit;
      }
    }
  }

  if (!exact_range) {"""),
    # Window reset at the start of RequestRanges.
    ("""  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),""",
     """  SCOPE_profile_cpu_f("gpu");

  if (REXCVAR_GET(sdnx_hot_pages)) {
    // SDNX perf: hot pages - forget them every window, so pages that calm down are cached and watched again.
    uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
    if (now_ms - hot_window_start_ms_ >= uint64_t(std::max(REXCVAR_GET(sdnx_hot_page_window_ms), int32_t(100)))) {
      auto global_lock = global_critical_region_.Acquire();
      std::fill(system_page_flags_hot_.begin(), system_page_flags_hot_.end(), uint64_t(0));
      std::fill(page_fault_count_.begin(), page_fault_count_.end(), uint8_t(0));
      hot_window_start_ms_ = now_ms;
    }
  }

  std::sort(merged_ranges.begin(), merged_ranges.end(),"""),
])
