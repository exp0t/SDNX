/**
 * Sleeping Dogs port (SDNX): native renderer, vertex / index buffer cache.
 *
 * Copying every draw's vertex buffer from guest memory (with the byte swap) and converting its indices was the
 * main per-draw CPU cost. Static geometry is now copied once into a device-local buffer and reused:
 *  - the guest pages are watched (physical write callbacks, the per-page versions of native_texture.cpp);
 *  - a global write epoch makes the per-draw check O(1) when nothing was written anywhere since the last check,
 *    and per draw (not per frame) because engines rewrite ring buffers in the middle of a frame;
 *  - an entry that changes again within a few frames is dynamic (UI, particles): it goes back to the per-draw
 *    upload and its pages are not watched any more (no write faults), retried after a while.
 * A replaced region is freed once the frames that may still read it have completed.
 */
#include <rex/graphics/sdnx/native_command_processor.h>

#include <algorithm>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::sdnx {

namespace {
constexpr VkDeviceSize kGeometryBufferSize = VkDeviceSize(192) << 20;
constexpr VkDeviceSize kGeometryAlignment = 256;
constexpr uint64_t kDynamicChangeFrames = 4;    // a change this soon after the previous one: dynamic
constexpr uint64_t kDynamicRetryFrames = 600;  // dynamic entries are tried as static again after this
constexpr uint64_t kEvictFrames = 300;         // entries unused for this long are dropped
constexpr uint64_t kHashedStableFrames = 8;   // content unchanged this long: hashed once per frame
constexpr uint32_t kHotFrames = 120;           // a page written this recently holds dynamic data
#ifdef __SWITCH__
constexpr VkDeviceSize kHashedBufferSize = VkDeviceSize(64) << 20;
#else
constexpr VkDeviceSize kHashedBufferSize = VkDeviceSize(128) << 20;
#endif
constexpr uint64_t kHashedEvictFrames = 120;  // content-hashed copies unused for this long are dropped

VkDeviceSize AllocateFrom(std::map<VkDeviceSize, VkDeviceSize>& free_blocks, VkDeviceSize size) {
  size = (size + kGeometryAlignment - 1) & ~(kGeometryAlignment - 1);
  for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it) {
    if (it->second < size) continue;
    VkDeviceSize offset = it->first, left = it->second - size;
    free_blocks.erase(it);
    if (left) free_blocks[offset + size] = left;
    return offset;
  }
  return UINT64_MAX;
}

void FreeTo(std::map<VkDeviceSize, VkDeviceSize>& free_blocks, VkDeviceSize offset, VkDeviceSize size) {
  size = (size + kGeometryAlignment - 1) & ~(kGeometryAlignment - 1);
  auto next = free_blocks.lower_bound(offset);
  if (next != free_blocks.end() && offset + size == next->first) {  // merge with the following block
    size += next->second;
    next = free_blocks.erase(next);
  }
  if (next != free_blocks.begin()) {  // merge with the preceding block
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      prev->second += size;
      return;
    }
  }
  free_blocks[offset] = size;
}
}  // namespace

bool NativeCommandProcessor::InitializeGeometry() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  uint32_t memory_type;
  // The page-watched geometry cache is off by default (see CachedGeometry): its 192 MB buffer is only made when it
  // is turned on. On the console that memory ran out after ~6 minutes of gameplay.
  geometry_free_.clear();
  if (std::getenv("SDNX_GEOCACHE")) {
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, kGeometryBufferSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, geometry_buffer_, geometry_memory_, &memory_type)) {
      return false;
    }
    geometry_free_[0] = kGeometryBufferSize;
  }
  if (ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kHashedBufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, hashed_buffer_, hashed_memory_, &memory_type)) {
    void* mapping = nullptr;
    device->functions().vkMapMemory(device->device(), hashed_memory_, 0, VK_WHOLE_SIZE, 0, &mapping);
    hashed_mapping_ = static_cast<uint8_t*>(mapping);
    hashed_free_.clear();
    if (hashed_mapping_) hashed_free_[0] = kHashedBufferSize;
  }
  return true;
}

NativeCommandProcessor::HashedGeometry* NativeCommandProcessor::HashedCopy(uint64_t key, uint64_t content,
                                                                           VkDeviceSize host_size, FillRef fill) {
  static const bool disabled = std::getenv("SDNX_NO_HASHCACHE") != nullptr;  // A/B measurements
  if (disabled || !hashed_mapping_ || !host_size || host_size > kHashedBufferSize / 8) return nullptr;
  auto [entry_ptr, inserted] = hashed_.try_emplace(key);
  HashedGeometry& entry = *entry_ptr;
  entry.last_used = frame_;
  if (!inserted && entry.offset != UINT64_MAX && entry.content == content && entry.size >= host_size) {
    ++stats_.hash_hits;
    entry.hashed_frame = frame_;
    return &entry;
  }
  // New or changed: the old copy may still be read by frames in flight.
  if (entry.offset != UINT64_MAX) {
    hashed_retired_[uint32_t(frame_ % kFramesInFlight)].push_back({entry.offset, entry.size});
    entry.offset = UINT64_MAX;
  }
  VkDeviceSize offset = AllocateFrom(hashed_free_, host_size);
  if (offset == UINT64_MAX) {
    hashed_.erase(key);
    return nullptr;
  }
  fill(hashed_mapping_ + offset);
  entry.hashed_frame = frame_;
  entry.content_since = frame_;
  entry.content = content;
  entry.offset = offset;
  entry.size = host_size;
  ++stats_.hash_fills;
  stats_.hash_fill_bytes += host_size;
  return &entry;
}

// A range drawn several times in a frame (shadow cascades, main pass, reflections) is hashed once per frame - but
// only when its content has not changed for a while: a scratch buffer refilled for every draw at the same address
// changes content all the time and is hashed on every draw.
NativeCommandProcessor::HashedGeometry* NativeCommandProcessor::HashedReuse(uint64_t key, VkDeviceSize host_size) {
  static const bool disabled = std::getenv("SDNX_NO_HASHREUSE") != nullptr;  // A/B measurements
  if (disabled) return nullptr;
  HashedGeometry* found = hashed_.find(key);
  if (!found) return nullptr;
  HashedGeometry& entry = *found;
  if (entry.offset == UINT64_MAX || entry.size < host_size || entry.hashed_frame != frame_ ||
      frame_ - entry.content_since < kHashedStableFrames) {
    return nullptr;
  }
  entry.last_used = frame_;
  ++stats_.hash_reuse;
  return &entry;
}

void NativeCommandProcessor::ShutdownGeometry() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  if (geometry_buffer_) dfn.vkDestroyBuffer(device->device(), geometry_buffer_, nullptr);
  if (geometry_memory_) dfn.vkFreeMemory(device->device(), geometry_memory_, nullptr);
  geometry_buffer_ = VK_NULL_HANDLE;
  geometry_memory_ = VK_NULL_HANDLE;
  if (hashed_buffer_) dfn.vkDestroyBuffer(device->device(), hashed_buffer_, nullptr);
  if (hashed_memory_) dfn.vkFreeMemory(device->device(), hashed_memory_, nullptr);
  hashed_buffer_ = VK_NULL_HANDLE;
  hashed_memory_ = VK_NULL_HANDLE;
  hashed_mapping_ = nullptr;
  hashed_.clear();
  hashed_free_.clear();
  for (auto& retired : hashed_retired_) retired.clear();
  geometry_.clear();
  geometry_free_.clear();
  for (auto& retired : geometry_retired_) retired.clear();
}

VkDeviceSize NativeCommandProcessor::AllocateGeometry(VkDeviceSize size) {
  size = (size + kGeometryAlignment - 1) & ~(kGeometryAlignment - 1);
  for (auto it = geometry_free_.begin(); it != geometry_free_.end(); ++it) {
    if (it->second < size) continue;
    VkDeviceSize offset = it->first, left = it->second - size;
    geometry_free_.erase(it);
    if (left) geometry_free_[offset + size] = left;
    return offset;
  }
  return UINT64_MAX;
}

void NativeCommandProcessor::FreeGeometry(VkDeviceSize offset, VkDeviceSize size) {
  size = (size + kGeometryAlignment - 1) & ~(kGeometryAlignment - 1);
  auto next = geometry_free_.lower_bound(offset);
  if (next != geometry_free_.end() && offset + size == next->first) {  // merge with the following block
    size += next->second;
    next = geometry_free_.erase(next);
  }
  if (next != geometry_free_.begin()) {  // merge with the preceding block
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      prev->second += size;
      return;
    }
  }
  geometry_free_[offset] = size;
}

void NativeCommandProcessor::ReleaseRetiredGeometry(uint32_t slot) {
  for (const auto& region : geometry_retired_[slot]) FreeGeometry(region.first, region.second);
  geometry_retired_[slot].clear();
  for (const auto& region : hashed_retired_[slot]) FreeTo(hashed_free_, region.first, region.second);
  hashed_retired_[slot].clear();
  // Eviction (every 64 frames): ring-buffer addresses seen once and geometry no longer drawn.
  if (frame_ % 64) return;
  hashed_.erase_if([&](uint64_t, HashedGeometry& entry) {
    if (frame_ - entry.last_used <= kHashedEvictFrames) return false;
    if (entry.offset != UINT64_MAX) hashed_retired_[slot].push_back({entry.offset, entry.size});
    return true;
  });
  for (auto it = geometry_.begin(); it != geometry_.end();) {
    if (frame_ - it->second.last_used > kEvictFrames) {
      if (it->second.offset != UINT64_MAX) geometry_retired_[slot].push_back({it->second.offset, it->second.size});
      it = geometry_.erase(it);
    } else {
      ++it;
    }
  }
}

bool NativeCommandProcessor::PagesHot(uint32_t first, uint32_t count) const {
  uint32_t now = uint32_t(frame_) + 1;  // callback_frame_ is frame_ + 1 (0 means never written)
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t written = page_write_frame_[(first + i) & 0x1FFFF].load(std::memory_order_relaxed);
    if (!written) continue;
    // First write: 120 frames (data loaded once, e.g. streaming); each further one doubles it, up to ~forever.
    uint32_t strikes = page_strikes_[(first + i) & 0x1FFFF].load(std::memory_order_relaxed);
    static const int hot_shift_max = std::getenv("SDNX_HOT_SHIFT") ? std::atoi(std::getenv("SDNX_HOT_SHIFT")) : 15;
    uint64_t period = uint64_t(kHotFrames) << std::min(strikes > 0 ? strikes - 1 : 0u, uint32_t(hot_shift_max));
    if (uint64_t(now - written) < period) {
      ++stats_.hot_checks;
      stats_.hot_age += now - written;
      stats_.hot_strikes += strikes;
      stats_.hot_strikes_max = std::max<uint64_t>(stats_.hot_strikes_max, strikes);
      return true;
    }
  }
  return false;
}

VkDeviceSize NativeCommandProcessor::CachedGeometry(uint64_t key, uint32_t address, uint32_t size,
                                                    VkDeviceSize host_size, FillRef fill) {
  // Off by default (SDNX_GEOCACHE=1 turns it on): in gameplay the vertex pages of this game are rewritten about once a
  // second, so every buffer was refused as hot (0 hits of ~3800 lookups/frame) and the lookup itself cost ~0.1 us/draw;
  // the content-hashed copies (HashedCopy / HashedReuse) serve all geometry.
  static const bool disabled = std::getenv("SDNX_GEOCACHE") == nullptr;
  if (disabled || !geometry_buffer_ || !size || !host_size || host_size > kGeometryBufferSize / 4) return UINT64_MAX;
  auto [entry_it, inserted] = geometry_.try_emplace(key);
  GeometryEntry& entry = entry_it->second;
  entry.last_used = frame_;
  if (inserted || entry.first_seen == frame_) {
    // Cached only when seen again in a later frame: data at an address used once (a ring buffer) is copied
    // for the draw directly, without filling the cache or watching its pages.
    if (inserted) entry.first_seen = frame_;
    ++stats_.geo_first;
    return UINT64_MAX;
  }
  uint64_t epoch = write_epoch_.load(std::memory_order_acquire);
  if (entry.dynamic) {
    if (frame_ - entry.dynamic_since < kDynamicRetryFrames) {
      ++stats_.geo_dynamic;
      ++stats_.geo_flagged;
      return UINT64_MAX;
    }
    entry.dynamic = false;
    entry.changes = 0;
  }
  if (entry.offset != UINT64_MAX) {
    if (entry.epoch == epoch) {
      ++stats_.geo_hits;
      return entry.offset;
    }
    if (PagesVersion(entry.page_first, entry.page_count) == entry.version) {
      entry.epoch = epoch;
      ++stats_.geo_hits;
      return entry.offset;
    }
    // Changed: the old copy may still be read by frames in flight.
    ++stats_.geo_changed;
    EnsureFrame();
    geometry_retired_[uint32_t(frame_ % kFramesInFlight)].push_back({entry.offset, entry.size});
    entry.offset = UINT64_MAX;
    bool recent = entry.changes && frame_ - entry.last_change_frame <= kDynamicChangeFrames;
    ++entry.changes;
    entry.last_change_frame = frame_;
    if (recent) {
      entry.dynamic = true;
      entry.dynamic_since = frame_;
      ++stats_.geo_dynamic;
      return UINT64_MAX;
    }
  }
  // (Re)load: watch first, then read, so a write from now on changes the version. Not on pages written recently:
  // that is dynamic data (and watching it would only add write faults).
  // A buffer refused for hot pages is not scanned again for a while (a scan of all its pages on every draw was a
  // large part of the vertex cost); the content-hashed copies serve it meanwhile.
  if (frame_ < entry.hot_until) {
    ++stats_.geo_dynamic;
    ++stats_.geo_hot;
    return UINT64_MAX;
  }
  entry.page_first = address >> 12;
  entry.page_count = ((address + size - 1) >> 12) - entry.page_first + 1;
  if (PagesHot(entry.page_first, entry.page_count)) {
    entry.hot_until = frame_ + 60;
    ++stats_.geo_dynamic;
    ++stats_.geo_hot;
    return UINT64_MAX;
  }
  memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
  entry.epoch = write_epoch_.load(std::memory_order_acquire);
  entry.version = PagesVersion(entry.page_first, entry.page_count);
  VkDeviceSize offset = AllocateGeometry(host_size);
  if (offset == UINT64_MAX) {
    // Full: treat as dynamic for a while (the per-draw path works, just slower).
    entry.dynamic = true;
    entry.dynamic_since = frame_;
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXGPU_WARN("[sdnx_native] geometry cache full ({} MB)", kGeometryBufferSize >> 20);
    }
    return UINT64_MAX;
  }
  EnsureFrame();
  uint8_t* staging_data = nullptr;
  VkDeviceSize staging = Upload(nullptr, host_size, 16, &staging_data);
  if (staging == UINT64_MAX) {
    FreeGeometry(offset, host_size);
    return UINT64_MAX;
  }
  fill(staging_data);
  EndRendering();  // copies are not allowed inside a rendering pass; the next pass begins with a full barrier
  VkBufferCopy copy = {staging, offset, host_size};
  vulkan_device()->functions().vkCmdCopyBuffer(cb_, upload_[uint32_t(frame_ % kFramesInFlight)].buffer,
                                               geometry_buffer_, 1, &copy);
  entry.offset = offset;
  entry.size = host_size;
  ++stats_.geo_uploads;
  stats_.geo_bytes += host_size;
  return offset;
}

}  // namespace rex::graphics::sdnx
