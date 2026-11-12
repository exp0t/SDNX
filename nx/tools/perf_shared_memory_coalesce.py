#!/usr/bin/env python3
"""Perf patch for the ReXGlue / masseffect-nx SDK shared memory (GPU copy of guest memory): re-upload whole stale runs.

When the guest writes a watched page, the write-watch fault invalidates a widened run of pages (up to the 64-page
block, 256 KB). Draws then request only the pieces they need, so one fault turned into ~5.5 separate uploads, each
with its own render pass break (SubmitBarriers) and its own VirtualProtect / RexGmProtect call when the range is
made valid again. Sleeping Dogs: ~56 000 protect calls/s, 15.7% of the "GPU Commands" thread in VirtualProtect
(desktop bench, 2026-11-08).

Now an upload range is extended to the neighbouring pages of the same 64-page block that are stale - were valid
(so their guest memory is mapped) and got invalidated by a CPU write, are not GPU-written - and the whole run is
uploaded and protected at once. Data uploaded is current CPU memory, so it is always correct.

Usage: perf_shared_memory_coalesce.py <sdk root>      (idempotent)
"""
import sys

root = sys.argv[1].rstrip("/\\")
mark = "SDNX perf: stale"


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
    "  std::vector<uint64_t> system_page_flags_valid_;\n",
    "  std::vector<uint64_t> system_page_flags_valid_;\n"
    "  // %s pages: were valid, then invalidated by a CPU write (memory known to be mapped).\n"
    "  std::vector<uint64_t> system_page_flags_stale_;\n" % mark,
)])

patch(root + "/src/graphics/shared_memory.cpp", [
    ("  system_page_flags_valid_.assign(num_system_page_flags_, 0);\n",
     "  system_page_flags_valid_.assign(num_system_page_flags_, 0);\n"
     "  system_page_flags_stale_.assign(num_system_page_flags_, 0);  // %s\n" % mark),
    ("  system_page_flags_valid_.clear();\n",
     "  system_page_flags_valid_.clear();\n  system_page_flags_stale_.clear();\n"),
    ("  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));\n",
     "  std::fill(system_page_flags_valid_.begin(), system_page_flags_valid_.end(), uint64_t(0));\n"
     "  std::fill(system_page_flags_stale_.begin(), system_page_flags_stale_.end(), uint64_t(0));\n"),
    ("      system_page_flags_valid_[i] |= valid_bits;\n",
     "      system_page_flags_valid_[i] |= valid_bits;\n      system_page_flags_stale_[i] &= ~valid_bits;\n"),
    ("    system_page_flags_valid_[i] &= ~invalidate_bits;\n",
     "    system_page_flags_stale_[i] |= system_page_flags_valid_[i] & invalidate_bits &\n"
     "                                   ~system_page_flags_valid_and_gpu_written_[i];\n"
     "    system_page_flags_valid_[i] &= ~invalidate_bits;\n"),
    # Extend the upload ranges (still under the global lock), then merge.
    ("""      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }
""", """      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
    // SDNX perf: stale runs - grow each upload range over the stale pages of its 64-page block.
    auto is_stale = [this](uint32_t page) {
      return (system_page_flags_stale_[page >> 6] >> (page & 63)) & 1;
    };
    for (std::pair<uint32_t, uint32_t>& upload_range : upload_ranges_) {
      uint32_t first = upload_range.first;
      uint32_t end = upload_range.first + upload_range.second;
      while ((first & 63) && is_stale(first - 1)) {
        --first;
      }
      while ((end & 63) && end < (kBufferSize >> page_size_log2_) && is_stale(end)) {
        ++end;
      }
      upload_range.first = first;
      upload_range.second = end - first;
    }
    if (upload_ranges_.size() > 1) {
      size_t write = 0;
      for (size_t i = 1; i < upload_ranges_.size(); ++i) {
        std::pair<uint32_t, uint32_t>& previous = upload_ranges_[write];
        const std::pair<uint32_t, uint32_t>& current = upload_ranges_[i];
        if (current.first <= previous.first + previous.second) {
          previous.second = std::max(previous.first + previous.second, current.first + current.second) -
                            previous.first;
        } else {
          upload_ranges_[++write] = current;
        }
      }
      upload_ranges_.resize(write + 1);
    }
  }
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_ranges_) {
    if (!EnsureHostGpuMemoryAllocated(upload_range.first << page_size_log2_,
                                      upload_range.second << page_size_log2_)) {
      return false;
    }
  }
"""),
])
