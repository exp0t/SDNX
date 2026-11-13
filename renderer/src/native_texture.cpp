/**
 * Sleeping Dogs port (SDNX): native renderer, textures and samplers (stage 2b).
 *
 * Guest textures are untiled and byte-swapped on the CPU into the frame's upload buffer and copied into host
 * images (all stored levels, 2D and cube). A texture is reloaded when the guest writes to its memory: the pages
 * are watched with the physical memory write callbacks, which bump per-page version counters. Render-to-texture
 * uses the resolved image of that address directly. Views (with the fetch constant swizzle) and samplers get
 * indices in the bindless heaps (sets 0-3), written while command buffers are recording (UPDATE_AFTER_BIND).
 * The shaders read the indices from the shared block: 2D at s*4, 3D at 64+s*4, cube at 128+s*4 (the sign of
 * each result component in bits 24-31), sampler at 192+s*4, 1/size at 360+s*8.
 */
#include <rex/graphics/sdnx/native_command_processor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/filesystem.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string/buffer.h>
#include <rex/system/xmemory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::sdnx {

namespace {

constexpr uint32_t kHeapSize = 8192;  // as in native_draw.cpp
constexpr uint32_t kPageSizeLog2 = 12;
constexpr uint32_t kPhysicalPages = 0x20000000u >> kPageSizeLog2;

// Xenos swizzles: 3 bits per destination component, 0-3 = R G B A, 4 = 0, 5 = 1.
constexpr uint32_t Swz(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | y << 3 | z << 6 | w << 9; }
constexpr uint32_t kSwizzleRGBA = Swz(0, 1, 2, 3), kSwizzleRRRR = Swz(0, 0, 0, 0), kSwizzleRGGG = Swz(0, 1, 1, 1),
                   kSwizzleRGBB = Swz(0, 1, 2, 2), kSwizzleBGRA = Swz(2, 1, 0, 3);

// kCtx1: Xbox 360 2-channel block format (normal maps), decoded on the CPU to R8G8 (4x4 texels per 8-byte block).
enum class Convert { kNone, k565, k1555, k4444, kDepth24, kCtx1 };
struct HostFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t swizzle = kSwizzleRGBA;  // guest component -> host component (like the SDK Vulkan texture cache)
  Convert convert = Convert::kNone;
};

HostFormat GetHostFormat(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8:
    case F::k_8_A:
      return {VK_FORMAT_R8_UNORM, kSwizzleRRRR};
    case F::k_1_5_5_5:
      return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, kSwizzleRGBA, Convert::k1555};
    case F::k_5_6_5:
      return {VK_FORMAT_R5G6B5_UNORM_PACK16, kSwizzleRGBB, Convert::k565};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return {VK_FORMAT_R8G8B8A8_UNORM, kSwizzleRGBA};
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, kSwizzleRGBA};
    case F::k_8_8:
      return {VK_FORMAT_R8G8_UNORM, kSwizzleRGGG};
    case F::k_4_4_4_4:
      return {VK_FORMAT_B4G4R4A4_UNORM_PACK16, kSwizzleRGBA, Convert::k4444};
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, kSwizzleRGBA};
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      return {VK_FORMAT_BC2_UNORM_BLOCK, kSwizzleRGBA};
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      return {VK_FORMAT_BC3_UNORM_BLOCK, kSwizzleRGBA};
    case F::k_DXN:
      return {VK_FORMAT_BC5_UNORM_BLOCK, kSwizzleRGGG};
    case F::k_DXT5A:
      return {VK_FORMAT_BC4_UNORM_BLOCK, kSwizzleRRRR};
    // Without it the floor of the night market (normal map) got the white placeholder: a white, lit-up road
    // with no confetti decals (2026-11-12).
    case F::k_CTX1:
      return {VK_FORMAT_R8G8_UNORM, kSwizzleRGGG, Convert::kCtx1};
    case F::k_16:
      return {VK_FORMAT_R16_UNORM, kSwizzleRRRR};
    case F::k_16_16:
      return {VK_FORMAT_R16G16_UNORM, kSwizzleRGGG};
    case F::k_16_16_16_16:
      return {VK_FORMAT_R16G16B16A16_UNORM, kSwizzleRGBA};
    case F::k_16_EXPAND:
    case F::k_16_FLOAT:
      return {VK_FORMAT_R16_SFLOAT, kSwizzleRRRR};
    case F::k_16_16_EXPAND:
    case F::k_16_16_FLOAT:
      return {VK_FORMAT_R16G16_SFLOAT, kSwizzleRGGG};
    case F::k_16_16_16_16_EXPAND:
    case F::k_16_16_16_16_FLOAT:
      return {VK_FORMAT_R16G16B16A16_SFLOAT, kSwizzleRGBA};
    case F::k_32_FLOAT:
      return {VK_FORMAT_R32_SFLOAT, kSwizzleRRRR};
    // Depth: normally the resolved R32_SFLOAT image of a depth resolve; from memory, the 24-bit unorm depth.
    case F::k_24_8:
      return {VK_FORMAT_R32_SFLOAT, kSwizzleRRRR, Convert::kDepth24};
    case F::k_24_8_FLOAT:
      return {VK_FORMAT_R32_SFLOAT, kSwizzleRRRR};  // TODO: 20e4 from memory (resolved images are float)
    case F::k_32_32_FLOAT:
      return {VK_FORMAT_R32G32_SFLOAT, kSwizzleRGGG};
    case F::k_32_32_32_32_FLOAT:
      return {VK_FORMAT_R32G32B32A32_SFLOAT, kSwizzleRGBA};
    default:
      return {};
  }
}

// The signed twin of a host texture format (a view of the same bits), VK_FORMAT_UNDEFINED if there is none.
VkFormat SignedFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R16_UNORM: return VK_FORMAT_R16_SNORM;
    case VK_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_SNORM;
    case VK_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_BC4_SNORM_BLOCK;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_BC5_SNORM_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
  }
}

void SwapInPlace(uint8_t* data, size_t size, xenos::Endian endian) {
  uint32_t* d = reinterpret_cast<uint32_t*>(data);
  size_t words = size / 4;
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i < words; ++i) d[i] = ((d[i] & 0x00FF00FFu) << 8) | ((d[i] >> 8) & 0x00FF00FFu);
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i < words; ++i) d[i] = __builtin_bswap32(d[i]);
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i < words; ++i) d[i] = (d[i] >> 16) | (d[i] << 16);
      break;
    default:
      break;
  }
}

void Convert16(uint8_t* data, size_t size, Convert convert) {
  uint16_t* p = reinterpret_cast<uint16_t*>(data);
  for (size_t i = 0; i < size / 2; ++i) {
    uint16_t v = p[i];
    switch (convert) {
      case Convert::k565:  // guest R 0-4, G 5-10, B 11-15 -> R 11-15, B 0-4
      case Convert::k1555:  // guest R 0-4, G 5-9, B 10-14, A 15 -> R 10-14, B 0-4
        if (convert == Convert::k565) {
          v = uint16_t((v >> 11) | (v & 0x07E0) | ((v & 0x1F) << 11));
        } else {
          v = uint16_t(((v >> 10) & 0x1F) | (v & 0x83E0) | ((v & 0x1F) << 10));
        }
        break;
      case Convert::k4444: {  // guest R 0-3, G 4-7, B 8-11, A 12-15 -> B 12-15, G 8-11, R 4-7, A 0-3
        uint16_t r = v & 0xF, g = (v >> 4) & 0xF, b = (v >> 8) & 0xF, a = (v >> 12) & 0xF;
        v = uint16_t(b << 12 | g << 8 | r << 4 | a);
        break;
      }
      default:
        break;
    }
    p[i] = v;
  }
}

VkSamplerAddressMode AddressMode(xenos::ClampMode mode) {
  switch (uint32_t(mode)) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2:
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;     // halfway: approximated
    case 3:
    case 5:
    case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;  // mirror to border: approximated
    case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

// Fetch constant fields that define where and how the texture is stored (not how it is sampled).
struct TextureKey {
  uint32_t base_page, mip_page, format, endian, width, height, depth, pitch, tiled, packed, dimension, stacked,
      mip_max;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Setup.

void NativeCommandProcessor::InitializeTextures() {
  page_versions_.reset(new std::atomic<uint32_t>[kPhysicalPages]);
  page_write_frame_.reset(new std::atomic<uint32_t>[kPhysicalPages]);
  page_strikes_.reset(new std::atomic<uint8_t>[kPhysicalPages]);
  for (uint32_t i = 0; i < kPhysicalPages; ++i) {
    page_versions_[i].store(0, std::memory_order_relaxed);
    page_write_frame_[i].store(0, std::memory_order_relaxed);
    page_strikes_[i].store(0, std::memory_order_relaxed);
  }
  write_watch_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(PhysicalWriteCallback, this);
}

void NativeCommandProcessor::ShutdownTextures() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkDevice vk = device->device();
  if (write_watch_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(write_watch_handle_);
    write_watch_handle_ = nullptr;
  }
  for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) ReleaseRetiredViews(slot);
  for (auto& it : views_) dfn.vkDestroyImageView(vk, it.second.view, nullptr);
  views_.clear();
  image_views_.clear();
  for (auto& it : textures_) DestroyImage(it.second.image);
  textures_.clear();
  for (auto& it : resolved_copies_) DestroyImage(it.second.image);
  resolved_copies_.clear();
  for (VkSampler sampler : sampler_objects_) dfn.vkDestroySampler(vk, sampler, nullptr);
  sampler_objects_.clear();
  samplers_.clear();
}

std::pair<uint32_t, uint32_t> NativeCommandProcessor::PhysicalWriteCallback(void* context, uint32_t start,
                                                                            uint32_t length, bool exact) {
  (void)exact;
  auto* self = static_cast<NativeCommandProcessor*>(context);
  if (length) {
    uint32_t first = (start & 0x1FFFFFFFu) >> kPageSizeLog2;
    uint32_t last = std::min(((start & 0x1FFFFFFFu) + length - 1) >> kPageSizeLog2, kPhysicalPages - 1);
    uint32_t frame = self->callback_frame_.load(std::memory_order_relaxed);
    for (uint32_t page = first; page <= last; ++page) {
      self->page_versions_[page].fetch_add(1, std::memory_order_relaxed);
      self->page_write_frame_[page].store(frame, std::memory_order_relaxed);
      uint8_t strikes = self->page_strikes_[page].load(std::memory_order_relaxed);
      if (strikes < 16) self->page_strikes_[page].store(strikes + 1, std::memory_order_relaxed);
    }
    self->write_epoch_.fetch_add(1, std::memory_order_release);
    self->write_callbacks_.fetch_add(1, std::memory_order_relaxed);
  }
  // Only the written pages may be unwatched: returning (0, UINT32_MAX) lets the memory system unprotect up to
  // 4 MB around the write, and later writes to other textures there would go unnoticed (stale glyphs, logos).
  return {start, length};
}

uint64_t NativeCommandProcessor::PagesVersion(uint32_t first, uint32_t count) const {
  uint64_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) {
    sum += page_versions_[(first + i) % kPhysicalPages].load(std::memory_order_relaxed);
  }
  return sum;
}

uint64_t NativeCommandProcessor::TexturePagesVersion(const Texture& texture) const {
  uint64_t sum = 0;
  for (uint32_t i = 0; i < texture.page_count; ++i) {
    sum += page_versions_[(texture.page_first + i) % kPhysicalPages].load(std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < texture.mip_page_count; ++i) {
    sum += page_versions_[(texture.mip_page_first + i) % kPhysicalPages].load(std::memory_order_relaxed);
  }
  return sum;
}

// ---------------------------------------------------------------------------------------------------------------
// Views and heap indices.

uint32_t NativeCommandProcessor::GetView(const Image& image, uint32_t heap, uint32_t levels, uint32_t layers,
                                         uint32_t swizzle, uint32_t host_swizzle, VkFormat view_format) {
  if (view_format == VK_FORMAT_UNDEFINED) view_format = image.format;
  last_texture_format_ = view_format;
  uint64_t key_data[3] = {uint64_t(reinterpret_cast<uintptr_t>(image.image)), uint64_t(heap) | uint64_t(levels) << 8,
                          uint64_t(swizzle) | uint64_t(host_swizzle) << 16 | uint64_t(view_format) << 32};
  uint64_t key = XXH3_64bits(key_data, sizeof(key_data));
  auto it = views_.find(key);
  if (it != views_.end()) return it->second.index;

  uint32_t index;
  if (!free_indices_[heap].empty()) {
    index = free_indices_[heap].back();
    free_indices_[heap].pop_back();
  } else if (next_index_[heap] < kHeapSize) {
    index = next_index_[heap]++;
  } else {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXGPU_WARN("[sdnx_native] texture heap {} is full ({} views)", heap, kHeapSize);
    }
    return 0;
  }
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkImageViewCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image.image;
  info.viewType = heap == 2 ? VK_IMAGE_VIEW_TYPE_CUBE : (heap == 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D);
  info.format = view_format;
  VkComponentSwizzle* components[4] = {&info.components.r, &info.components.g, &info.components.b,
                                       &info.components.a};
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest = (swizzle >> (3 * i)) & 7;
    if (guest < 4) {
      *components[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + ((host_swizzle >> (3 * guest)) & 3));
    } else {
      *components[i] = guest == 5 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
    }
  }
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, heap == 2 ? 6u : std::min(layers, 1u)};
  TextureView view;
  view.heap = heap;
  view.index = index;
  if (dfn.vkCreateImageView(device->device(), &info, nullptr, &view.view) != VK_SUCCESS) {
    free_indices_[heap].push_back(index);
    return 0;
  }
  VkDescriptorImageInfo image_info = {VK_NULL_HANDLE, view.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = heap_sets_[heap];
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image_info;
  dfn.vkUpdateDescriptorSets(device->device(), 1, &write, 0, nullptr);
  views_.emplace(key, view);
  image_views_[image.image].push_back(key);
  return index;
}

void NativeCommandProcessor::RetireViews(VkImage image) {
  auto it = image_views_.find(image);
  if (it == image_views_.end()) return;
  std::vector<TextureView>& retired = retired_views_[uint32_t(frame_ % kFramesInFlight)];
  for (uint64_t key : it->second) {
    auto view = views_.find(key);
    if (view == views_.end()) continue;
    retired.push_back(view->second);
    views_.erase(view);
  }
  image_views_.erase(it);
}

void NativeCommandProcessor::ReleaseRetiredViews(uint32_t slot) {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  for (const TextureView& view : retired_views_[slot]) {
    device->functions().vkDestroyImageView(device->device(), view.view, nullptr);
    free_indices_[view.heap].push_back(view.index);
  }
  retired_views_[slot].clear();
}

// ---------------------------------------------------------------------------------------------------------------
// Samplers.

bool NativeCommandProcessor::FormatFilterable(VkFormat format) {
  auto it = filterable_formats_.find(format);
  if (it != filterable_formats_.end()) return it->second;
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  VkFormatProperties properties = {};
  device->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(device->physical_device(), format,
                                                                             &properties);
  bool filterable = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  if (!filterable) {
    REXGPU_WARN("[sdnx_native] format {} cannot be sampled with linear filtering here: point sampling it", int(format));
  }
  filterable_formats_[format] = filterable;
  return filterable;
}

uint32_t NativeCommandProcessor::GetSamplerDescriptor(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                      const ParsedTextureFetchInstruction& instr) {
  using xenos::TextureFilter;
  const auto& attr = instr.attributes;
  TextureFilter mag = attr.mag_filter == TextureFilter::kUseFetchConst ? fetch.mag_filter : attr.mag_filter;
  TextureFilter min = attr.min_filter == TextureFilter::kUseFetchConst ? fetch.min_filter : attr.min_filter;
  TextureFilter mip = attr.mip_filter == TextureFilter::kUseFetchConst ? fetch.mip_filter : attr.mip_filter;
  // Linear filtering of a format the device cannot filter (R32_SFLOAT depth on some drivers) is undefined:
  // on the console the shadows came out black. Point sampling instead (last_texture_format_: BindTextures sets it
  // with the texture of this fetch just before).
  if (last_texture_format_ != VK_FORMAT_UNDEFINED && !FormatFilterable(last_texture_format_)) {
    mag = TextureFilter::kPoint;
    min = TextureFilter::kPoint;
    if (mip == TextureFilter::kLinear) mip = TextureFilter::kPoint;
  }
  int32_t bias = int32_t(fetch.lod_bias) + int32_t(std::lround(attr.lod_bias * 32.0f));
  bias = std::clamp(bias, -512, 511);
  uint64_t key = uint64_t(mag == TextureFilter::kLinear) | uint64_t(min == TextureFilter::kLinear) << 1 |
                 uint64_t(mip == TextureFilter::kLinear ? 1 : (mip == TextureFilter::kBaseMap ? 2 : 0)) << 2 |
                 uint64_t(fetch.clamp_x) << 4 | uint64_t(fetch.clamp_y) << 7 | uint64_t(fetch.clamp_z) << 10 |
                 uint64_t(fetch.border_color) << 13 | uint64_t(uint32_t(bias) & 0x3FF) << 16;
  auto it = samplers_.find(key);
  if (it != samplers_.end()) return it->second;
  static const bool trace_prim = std::getenv("SDNX_TRACE_PRIM") != nullptr;
  if (trace_prim) {
    REXGPU_INFO("[sdnx_sampler] new: mag {} min {} mip {} clamp {} {} {} border {} bias {}", uint32_t(mag),
                uint32_t(min), uint32_t(mip), uint32_t(fetch.clamp_x), uint32_t(fetch.clamp_y),
                uint32_t(fetch.clamp_z), uint32_t(fetch.border_color), bias);
  }
  if (sampler_objects_.size() + 1 >= kHeapSize) return 0;

  VkSamplerCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = mag == TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = min == TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = mip == TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = AddressMode(fetch.clamp_x);
  info.addressModeV = AddressMode(fetch.clamp_y);
  info.addressModeW = AddressMode(fetch.clamp_z);
  info.mipLodBias = float(bias) / 32.0f;
  info.minLod = 0.0f;
  info.maxLod = mip == TextureFilter::kBaseMap ? 0.25f : VK_LOD_CLAMP_NONE;
  switch (uint32_t(fetch.border_color)) {
    case 0: info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
    case 1: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
    default: info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
  }
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkSampler sampler;
  if (dfn.vkCreateSampler(device->device(), &info, nullptr, &sampler) != VK_SUCCESS) return 0;
  sampler_objects_.push_back(sampler);
  uint32_t index = uint32_t(sampler_objects_.size());  // 0 is the placeholder sampler
  VkDescriptorImageInfo image_info = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = heap_sets_[3];
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  write.pImageInfo = &image_info;
  dfn.vkUpdateDescriptorSets(device->device(), 1, &write, 0, nullptr);
  samplers_.emplace(key, index);
  return index;
}

// ---------------------------------------------------------------------------------------------------------------
// Loading.

bool NativeCommandProcessor::LoadTexture(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  xenos::TextureFormat format = fetch.format;
  HostFormat host = GetHostFormat(format);
  const FormatInfo* info = FormatInfo::Get(format);
  bool cube = fetch.dimension == xenos::DataDimension::kCube;
  bool is_3d = fetch.dimension == xenos::DataDimension::k3D;
  if (!host.format || !info || !base_page) {
    texture.failed = true;
    ++stats_.tex_unsupported;
    if (unsupported_formats_[uint32_t(format) | uint32_t(fetch.dimension) << 8]++ == 0) {
      REXGPU_WARN("[sdnx_native] texture not supported yet: format {} dimension {} base page {:#x}",
                  uint32_t(format), uint32_t(fetch.dimension), base_page);
    }
    return false;
  }
  uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  uint32_t array_size = depth_minus_1 + 1;  // 6 for cubes; stacked 2D: only slice 0 is used
  uint32_t layers = cube ? 6 : 1;
  uint32_t depth = is_3d ? depth_minus_1 + 1 : 1;
  uint32_t levels = mip_max + 1;
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, width, height, array_size, fetch.tiled, format, fetch.packed_mips, true, mip_max);
  uint32_t bw = info->block_width, bh = info->block_height, bpb = info->bytes_per_block();
  uint32_t bpb_log2 = bpb >= 16 ? 4 : (bpb >= 8 ? 3 : (bpb >= 4 ? 2 : (bpb >= 2 ? 1 : 0)));

  // Watch the memory before reading it: a write from now on makes the version differ.
  uint32_t base_address = base_page << kPageSizeLog2, mip_address = mip_page << kPageSizeLog2;
  uint32_t base_extent = layout.base.level_data_extent_bytes;
  uint32_t mip_extent = levels > 1 ? layout.mips_total_extent_bytes : 0;
  texture.page_first = base_page;
  texture.page_count = (base_extent + (1u << kPageSizeLog2) - 1) >> kPageSizeLog2;
  texture.mip_page_first = mip_page;
  texture.mip_page_count = mip_extent ? (mip_extent + (1u << kPageSizeLog2) - 1) >> kPageSizeLog2 : 0;
  memory_->EnablePhysicalMemoryAccessCallbacks(base_address, base_extent, true, false);
  if (mip_extent) memory_->EnablePhysicalMemoryAccessCallbacks(mip_address, mip_extent, true, false);
  uint64_t version = TexturePagesVersion(texture);
  // Hashed after the watch is armed and before the data is read: a write from here on (video frames are decoded
  // into their textures while they are loaded) changes the version, and the next check sees different data.
  const uint64_t content_hash = TextureContentHash(texture);

  // Host size of all levels.
  VkDeviceSize total = 0;
  for (uint32_t level = 0; level < levels; ++level) {
    uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
    uint32_t ld = std::max(depth >> level, 1u);
    // CTX1 grows from 8 bytes to 16 R8G8 texels (32 bytes) per block.
    uint32_t host_bpb = host.convert == Convert::kCtx1 ? bw * bh * 2 : bpb;
    total += VkDeviceSize((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * host_bpb * layers * ld;
  }
  const bool ctx1 = host.convert == Convert::kCtx1;
  std::vector<uint8_t> ctx1_blocks;
  uint8_t* dst = nullptr;
  VkDeviceSize staging = Upload(nullptr, total, 16, &dst);
  if (staging == UINT64_MAX) return false;  // the frame's upload buffer is full: next frame

  std::vector<VkBufferImageCopy> regions;
  std::vector<uint8_t>& swapped = texture_scratch_;  // reused: no allocation and zero fill per load
  VkDeviceSize offset = 0;
  uint32_t packed = layout.packed_level;
  for (uint32_t level = 0; level < levels; ++level) {
    bool is_base = level == 0;
    uint32_t stored = std::min(level, packed);
    const texture_util::TextureGuestLayout::Level& guest = is_base ? layout.base : layout.mips[stored];
    uint32_t level_address = is_base ? base_address : mip_address + layout.mip_offsets_bytes[stored];
    uint32_t ox = 0, oy = 0, oz = 0;
    if (level >= packed) texture_util::GetPackedMipOffset(width, height, depth, format, level, ox, oy, oz);
    uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
    uint32_t ld = std::max(depth >> level, 1u);
    uint32_t slice_rows = guest.z_slice_stride_block_rows;
    uint32_t lbw = (lw + bw - 1) / bw, lbh = (lh + bh - 1) / bh;
    uint32_t pitch_blocks = guest.row_pitch_bytes / bpb;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      uint32_t slice_address = level_address + layer * guest.array_slice_stride_bytes;
      uint32_t extent = (guest.array_slice_data_extent_bytes + 3) & ~3u;
      const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(slice_address);
      if (!src || !extent) return false;
      swapped.resize(extent);
      std::memcpy(swapped.data(), src, extent);
      SwapInPlace(swapped.data(), extent, fetch.endianness);
      uint8_t* host_out = dst + offset;
      // CTX1: untile the blocks first, then decode them into host_out.
      if (ctx1) ctx1_blocks.resize(size_t(lbw) * lbh * ld * bpb);
      uint8_t* out = ctx1 ? ctx1_blocks.data() : host_out;
      for (uint32_t z = 0; z < ld; ++z) {
        for (uint32_t y = 0; y < lbh; ++y) {
          for (uint32_t x = 0; x < lbw; ++x) {
            uint32_t at;
            if (is_3d) {
              at = fetch.tiled ? uint32_t(texture_util::GetTiledOffset3D(int32_t(ox + x), int32_t(oy + y),
                                                                         int32_t(oz + z), pitch_blocks, slice_rows,
                                                                         bpb_log2))
                               : ((oz + z) * slice_rows + oy + y) * guest.row_pitch_bytes + (ox + x) * bpb;
            } else {
              at = fetch.tiled ? uint32_t(texture_util::GetTiledOffset2D(int32_t(ox + x), int32_t(oy + y),
                                                                         pitch_blocks, bpb_log2))
                               : (oy + y) * guest.row_pitch_bytes + (ox + x) * bpb;
            }
            uint8_t* to = out + ((size_t(z) * lbh + y) * lbw + x) * bpb;
            if (at + bpb <= extent) {
              std::memcpy(to, swapped.data() + at, bpb);
            } else {
              std::memset(to, 0, bpb);
            }
          }
        }
      }
      size_t size = size_t(lbw) * lbh * ld * bpb;
      // SDNX_DUMP_TEX=0x...: level 0 of the texture at that address as loaded (DXT1/3/5 as .dds, others raw) next
      // to the executable, to check the untiling.
      static const uint32_t dump_tex =
          std::getenv("SDNX_DUMP_TEX") ? uint32_t(std::strtoul(std::getenv("SDNX_DUMP_TEX"), nullptr, 16)) : 0;
      if (dump_tex && level == 0 && layer == 0 && base_address == dump_tex) {
        const char* fourcc = format == xenos::TextureFormat::k_DXT1        ? "DXT1"
                             : format == xenos::TextureFormat::k_DXT2_3    ? "DXT3"
                             : format == xenos::TextureFormat::k_DXT4_5    ? "DXT5"
                                                                           : nullptr;
        std::filesystem::path path = rex::filesystem::GetExecutableFolder() /
                                     fmt::format("tex_{:08x}.{}", dump_tex, fourcc ? "dds" : "bin");
        if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
          if (fourcc) {
            uint32_t header[32] = {};
            header[0] = 0x20534444;  // "DDS "
            header[1] = 124;
            header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000;
            header[3] = lh;
            header[4] = lw;
            header[5] = uint32_t(size);
            header[19] = 32;
            header[20] = 0x4;
            std::memcpy(&header[21], fourcc, 4);
            header[27] = 0x1000;
            std::fwrite(header, 4, 32, f);
          }
          std::fwrite(out, 1, size, f);
          std::fclose(f);
          REXGPU_INFO("[sdnx_native] dumped texture {:#x} format {} {}x{} tiled {} pitch {} -> {}", dump_tex,
                      uint32_t(format), lw, lh, uint32_t(fetch.tiled), uint32_t(fetch.pitch), path.string());
        }
      }
      if (ctx1) {
        const uint32_t row_bytes = lbw * bw * 2;
        for (uint32_t z = 0; z < ld; ++z) {
          for (uint32_t y = 0; y < lbh; ++y) {
            for (uint32_t x = 0; x < lbw; ++x) {
              texture_conversion::ConvertTexelCTX1ToR8G8(
                  xenos::Endian::kNone, host_out + (size_t(z) * lbh * bh + y * bh) * row_bytes + x * bw * 2,
                  out + ((size_t(z) * lbh + y) * lbw + x) * bpb, row_bytes);
            }
          }
        }
        size = size_t(row_bytes) * lbh * bh * ld;
      } else if (host.convert == Convert::kDepth24) {
        uint32_t* p32 = reinterpret_cast<uint32_t*>(out);
        for (size_t i = 0; i < size / 4; ++i) {
          float depth = float(p32[i] >> 8) * (1.0f / 16777215.0f);
          std::memcpy(&p32[i], &depth, 4);
        }
      } else if (host.convert != Convert::kNone) {
        Convert16(out, size, host.convert);
      }
      VkBufferImageCopy region = {};
      region.bufferOffset = staging + offset;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
      region.imageExtent = {lw, lh, ld};
      if (ctx1) {
        // Decoded in whole blocks: rows of lbw * 4 texels, lbh * 4 rows.
        region.bufferRowLength = lbw * bw;
        region.bufferImageHeight = lbh * bh;
      }
      regions.push_back(region);
      offset += size;
    }
  }

  // The host image (recreated when the size or format changes).
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  Image& image = texture.image;
  if (image.image &&
      (image.format != host.format || image.width != width || image.height != height || texture.levels != levels ||
       texture.layers != layers || texture.depth != depth)) {
    RetireImage(image);
  }
  EndRendering();
  if (!image.image) {
    VkImageCreateInfo create = {};
    create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    // Signed fetch constants read the same bits through an SNORM view.
    if (SignedFormat(host.format)) create.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    create.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    create.format = host.format;
    create.extent = {width, height, depth};
    create.mipLevels = levels;
    create.arrayLayers = layers;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, create, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                          image.image, image.memory)) {
      texture.failed = true;
      REXGPU_ERROR("[sdnx_native] could not create a {}x{} texture (format {})", width, height, uint32_t(format));
      return false;
    }
    image.format = host.format;
    image.width = width;
    image.height = height;
    texture.levels = levels;
    texture.layers = layers;
    texture.depth = depth;
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    dfn.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
    image.initialized = true;
  }
  // Earlier draws (also of frames still in flight - same queue) may sample the old contents.
  FullBarrier();
  dfn.vkCmdCopyBufferToImage(cb_, upload_[uint32_t(frame_ % kFramesInFlight)].buffer, image.image,
                             VK_IMAGE_LAYOUT_GENERAL, uint32_t(regions.size()), regions.data());
  FullBarrier();
  if (is_3d) {
    static int logged_3d = 0;
    if (logged_3d < 12) {
      ++logged_3d;
      uint32_t nonzero = 0;
      for (VkDeviceSize i = 0; i < total; ++i) nonzero += dst[i] != 0;
      REXGPU_INFO("[sdnx_native] 3D texture {:#x}: {}x{}x{} format {} tiled {} endian {} pitch {} levels {} extent "
                  "{} bytes, row pitch {}, slice rows {}; host {} bytes, {} non-zero; first {:02X} {:02X} {:02X} {:02X}",
                  base_address, width, height, depth, uint32_t(format), uint32_t(fetch.tiled),
                  uint32_t(fetch.endianness), uint32_t(fetch.pitch), levels, layout.base.array_slice_data_extent_bytes,
                  layout.base.row_pitch_bytes, layout.base.z_slice_stride_block_rows, uint64_t(total), nonzero, dst[0],
                  dst[1], dst[2], dst[3]);
    }
  }
  texture.version = version;
  texture.content_hash = content_hash;
  texture.validated_frame = frame_;
  texture.loaded = true;
  ++stats_.tex_loads;
  stats_.tex_bytes += total;
  return true;
}

uint64_t NativeCommandProcessor::TextureContentHash(const Texture& texture) {
  uint64_t hash = 0;
  const uint32_t ranges[2][2] = {{texture.page_first, texture.page_count},
                                 {texture.mip_page_first, texture.mip_page_count}};
  for (const auto& range : ranges) {
    if (!range[1]) continue;
    const uint8_t* p = memory_->TranslatePhysical<const uint8_t*>(range[0] << kPageSizeLog2);
    if (!p) continue;
    hash = XXH3_64bits_withSeed(p, size_t(range[1]) << kPageSizeLog2, hash);
  }
  return hash;
}

bool NativeCommandProcessor::OverlayResolvedFaces(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch) {
  if (!texture.loaded || !texture.image.image || texture.layers != 6) return false;
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  texture_util::TextureGuestLayout layout =
      texture_util::GetGuestTextureLayout(fetch.dimension, fetch.pitch, width, height, depth_minus_1 + 1,
                                          fetch.tiled, fetch.format, fetch.packed_mips, true, mip_max);
  uint32_t base_address = base_page << kPageSizeLog2, stride = layout.base.array_slice_stride_bytes;
  const Image* faces[6] = {};
  uint64_t generation = 0;
  bool any = false, swapped = false;
  for (uint32_t f = 0; f < 6; ++f) {
    uint32_t address = base_address + f * stride;
    auto it = resolved_.find(address);
    if (it == resolved_.end() || !it->second.image || it->second.format != texture.image.format ||
        it->second.width < width || it->second.height < height) {
      continue;
    }
    faces[f] = &it->second;
    generation = generation * 1000003 + resolved_generation_[address] + f + 1;
    auto swap_it = resolved_swap_.find(address);
    swapped |= swap_it != resolved_swap_.end() && swap_it->second;
    any = true;
  }
  if (!any) return false;
  texture.faces_resolved = true;
  texture.faces_swapped = swapped;
  if (generation == texture.face_generation) return true;
  texture.face_generation = generation;
  EndRendering();
  FullBarrier();
  for (uint32_t f = 0; f < 6; ++f) {
    if (!faces[f]) continue;
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
    region.extent = {width, height, 1};
    vulkan_device()->functions().vkCmdCopyImage(cb_, faces[f]->image, VK_IMAGE_LAYOUT_GENERAL, texture.image.image,
                                                VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  }
  FullBarrier();
  return true;
}

uint32_t NativeCommandProcessor::GetTextureDescriptor(uint32_t fetch_constant, uint32_t& heap, float inv_size[2]) {
  xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(fetch_constant);
  heap = 0;
  if (fetch.type != xenos::FetchConstantType::kTexture) return 0;
  switch (fetch.dimension) {
    case xenos::DataDimension::k3D: heap = 1; break;
    case xenos::DataDimension::kCube: heap = 2; break;
    default: heap = 0; break;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1, &base_page,
                                                 &mip_page, &mip_min, &mip_max);
  inv_size[0] = 1.0f / float(width_minus_1 + 1);
  inv_size[1] = 1.0f / float(height_minus_1 + 1);
  uint32_t swizzle = fetch.swizzle;
  uint32_t index = 0, host_swizzle = kSwizzleRGBA;
  bool signed_view = false;

  // Render to texture: the resolved image of that address.
  auto resolved = heap == 0 ? resolved_.find(base_page << kPageSizeLog2) : resolved_.end();
  if (resolved != resolved_.end() && resolved->second.image) {
    ++stats_.tex_resolved;
    static const bool trace_resolve = std::getenv("SDNX_TRACE_RESOLVE") != nullptr;
    static std::unordered_map<uint64_t, int> traced;
    uint64_t trace_key = uint64_t(base_page) | uint64_t(fetch.format) << 20 | uint64_t(uint32_t(fetch.exp_adjust) & 63) << 26;
    if (trace_resolve && traced[trace_key]++ == 0) {
      REXGPU_INFO("[sdnx_resolve] sampled {:#x} as format {} {}x{} exp_adjust={} signs={}{}{}{} swizzle={:#x} "
                  "num_format={} (image {}x{} host {})",
                  base_page << kPageSizeLog2, uint32_t(fetch.format), width_minus_1 + 1, height_minus_1 + 1,
                  int32_t(fetch.exp_adjust), uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                  uint32_t(fetch.sign_w), uint32_t(fetch.swizzle), uint32_t(fetch.num_format),
                  resolved->second.width, resolved->second.height, int(resolved->second.format));
    }
    const Image* source = &resolved->second;
    uint32_t tw = width_minus_1 + 1, th = height_minus_1 + 1;
    // Other resolves inside this texture's memory (see ResolvedPart): only possible when the texture is taller
    // than what was resolved to its base.
    uint64_t copy_key = uint64_t(base_page) | uint64_t(tw) << 20 | uint64_t(th) << 34 |
                        uint64_t(uint32_t(source->format) & 0xFF) << 48;
    auto copy_it = resolved_copies_.find(copy_key);
    bool need_copy = source->width != tw || source->height != th || copy_it != resolved_copies_.end();
    if (need_copy) {
      ResolvedCopy& copy = resolved_copies_[copy_key];
      // Parts are found again every frame (which ones are fresh changes) or when resolves were added.
      if (copy.parts_resolved_count != resolved_.size() || copy.parts_frame != frame_) {
        copy.parts_resolved_count = resolved_.size();
        copy.parts_frame = frame_;
        const FormatInfo* format_info = FormatInfo::Get(fetch.format);
        uint32_t bytes_per_pixel = format_info ? std::max(format_info->bits_per_pixel / 8, 1u) : 4;
        uint32_t pitch_pixels = fetch.pitch ? uint32_t(fetch.pitch) << 5 : ((tw + 31) & ~31u);
        uint64_t row_bytes = uint64_t(pitch_pixels) * bytes_per_pixel;
        uint32_t base_address = base_page << kPageSizeLog2;
        const uint64_t base_frame = resolved_frame_[base_address];
        std::vector<ResolvedPart> parts;
        for (const auto& other : resolved_) {
          if (!other.second.image || other.second.format != source->format) continue;
          if (other.first < base_address || other.first >= base_address + row_bytes * th) continue;
          // (A filter keeping only parts resolved in the frame of the base or the one before was tried against the
          // right-edge strip, which it did not fix; it is suspected of the white half-screen flashes seen on the
          // console when frames are slow, as the G-buffer is two resolved halves. Removed, 2026-11-13.)
          (void)base_frame;
          uint64_t delta = other.first - base_address;
          uint32_t y = uint32_t(delta / row_bytes), x = uint32_t((delta % row_bytes) / bytes_per_pixel);
          if (x < tw && y < th) parts.push_back({other.first, x, y});
        }
        // Ascending addresses: a later part overwrites what an earlier (larger than needed) image covers.
        std::sort(parts.begin(), parts.end(),
                  [](const ResolvedPart& l, const ResolvedPart& r) { return l.address < r.address; });
        bool same = parts.size() == copy.parts.size();
        for (size_t i = 0; same && i < parts.size(); ++i) {
          same = parts[i].address == copy.parts[i].address && parts[i].x == copy.parts[i].x &&
                 parts[i].y == copy.parts[i].y;
        }
        if (!same) {
          copy.parts = std::move(parts);
          copy.generation = ~0ull;
        }
      }
      if (copy.image.image && copy.image.format != source->format) RetireImage(copy.image);
      if (!copy.image.image) {
        EndRendering();
        if (!CreateImage(copy.image, source->format, tw, th, false)) {
          resolved_copies_.erase(copy_key);
          return 0;
        }
        InitializeLayout(copy.image);
        copy.generation = ~0ull;
      }
      uint64_t generation = 0;
      for (const ResolvedPart& part : copy.parts) generation += resolved_generation_[part.address];
      if (copy.generation != generation) {
        EndRendering();
        FullBarrier();
        for (const ResolvedPart& part : copy.parts) {
          auto part_it = resolved_.find(part.address);
          if (part_it == resolved_.end() || !part_it->second.image) continue;
          VkImageCopy region = {};
          region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.dstSubresource = region.srcSubresource;
          region.dstOffset = {int32_t(part.x), int32_t(part.y), 0};
          region.extent = {std::min(tw - part.x, part_it->second.width), std::min(th - part.y, part_it->second.height),
                           1};
          vulkan_device()->functions().vkCmdCopyImage(cb_, part_it->second.image, VK_IMAGE_LAYOUT_GENERAL,
                                                      copy.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        }
        FullBarrier();
        copy.generation = generation;
      }
      source = &copy.image;
    }
    // Depth textures read the R32_SFLOAT depth of a depth resolve in every component the format has.
    // A resolve with copy_dest_swap stored red and blue swapped in memory (the image holds them unswapped).
    auto swap_it = resolved_swap_.find(base_page << kPageSizeLog2);
    const bool swapped = swap_it != resolved_swap_.end() && swap_it->second;
    uint32_t resolved_host_swizzle =
        (fetch.format == xenos::TextureFormat::k_24_8 || fetch.format == xenos::TextureFormat::k_24_8_FLOAT)
            ? kSwizzleRRRR
            : (swapped ? kSwizzleBGRA : kSwizzleRGBA);
    index = GetView(*source, 0, 1, 1, swizzle, resolved_host_swizzle);
  } else {
    TextureKey key = {base_page, mip_page, uint32_t(fetch.format), uint32_t(fetch.endianness), width_minus_1,
                      height_minus_1, depth_minus_1, fetch.pitch, fetch.tiled, fetch.packed_mips,
                      uint32_t(fetch.dimension), fetch.stacked, mip_max};
    Texture& texture = textures_[XXH3_64bits(&key, sizeof(key))];
    static const bool trace_resolve = std::getenv("SDNX_TRACE_RESOLVE") != nullptr;
    if (trace_resolve && !texture.loaded && !texture.failed) {
      uint32_t address = base_page << kPageSizeLog2;
      for (const auto& r : resolved_extent_) {
        if (address > r.first && address < r.first + r.second) {
          REXGPU_INFO("[sdnx_resolve] texture {:#x} format {} {}x{} is inside the resolve to {:#x} (+{:#x})", address,
                      uint32_t(fetch.format), width_minus_1 + 1, height_minus_1 + 1, r.first, address - r.first);
        }
      }
    }
    if (texture.failed) return 0;
    if (!texture.loaded) {
      if (!LoadTexture(texture, fetch)) return 0;
    } else if (texture.validated_frame != frame_) {
      texture.validated_frame = frame_;
      uint64_t version = TexturePagesVersion(texture);
      if (version != texture.version) {
        static const bool no_hash = std::getenv("SDNX_NO_TEXHASH") != nullptr;  // A/B: reload on every write
        if (!no_hash && TextureContentHash(texture) == texture.content_hash) {
          // The pages were written, the texture data was not. The write watch is one-shot: arm it again, or the
          // next real change is never seen (the intro videos stayed on their first, black frame).
          memory_->EnablePhysicalMemoryAccessCallbacks(texture.page_first << kPageSizeLog2,
                                                       texture.page_count << kPageSizeLog2, true, false);
          if (texture.mip_page_count) {
            memory_->EnablePhysicalMemoryAccessCallbacks(texture.mip_page_first << kPageSizeLog2,
                                                         texture.mip_page_count << kPageSizeLog2, true, false);
          }
          texture.version = TexturePagesVersion(texture);
          if (TextureContentHash(texture) != texture.content_hash) LoadTexture(texture, fetch);
        } else {
          LoadTexture(texture, fetch);
        }
      }
    }
    host_swizzle = GetHostFormat(fetch.format).swizzle;
    if (heap == 2 && OverlayResolvedFaces(texture, fetch) && texture.faces_swapped &&
        host_swizzle == kSwizzleRGBA) {
      host_swizzle = kSwizzleBGRA;
    }
    // A signed component read: the whole view is SNORM (mixed signed / unsigned components are approximated).
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    xenos::TextureSign data_signs[4] = {fetch.sign_x, fetch.sign_y, fetch.sign_z, fetch.sign_w};
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t guest = (swizzle >> (3 * i)) & 7;
      if (guest < 4 && data_signs[guest] == xenos::TextureSign::kSigned) {
        view_format = SignedFormat(texture.image.format);
      }
    }
    signed_view = view_format != VK_FORMAT_UNDEFINED;
    index = GetView(texture.image, heap, texture.levels, texture.layers, swizzle, host_swizzle, view_format);
  }
  if (!index) return 0;
  // Signs of the result components (the swizzle selects the data component).
  xenos::TextureSign signs[4] = {fetch.sign_x, fetch.sign_y, fetch.sign_z, fetch.sign_w};
  uint32_t packed_signs = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t guest = (swizzle >> (3 * i)) & 7;
    uint32_t sign = guest < 4 ? uint32_t(signs[guest]) : 0;
    if (sign == 1) {
      if (!signed_view) ++stats_.tex_signed;  // no SNORM twin (or a resolved image): read as unsigned
      sign = 0;  // the shader has nothing to do for it (the SNORM view already gives signed values)
    }
    packed_signs |= sign << (2 * i);
  }
  return index | packed_signs << 24;
}

// Textures were never freed: every texture the game ever sampled kept its image, and as the world streamed the
// process ran out of memory after ~6 minutes of gameplay on the console (malloc headroom 301 -> 20 MB, then
// std::bad_alloc and failed image creations, 2026-11-13). Every 64 frames, textures not sampled for kTextureEvictFrames
// (validated_frame is set at the first use in each frame) are retired; sampled again later, they are reloaded.
void NativeCommandProcessor::EvictTextures() {
  constexpr uint64_t kTextureEvictFrames = 600;
  if (frame_ % 64) return;
  size_t evicted = 0;
  for (auto it = textures_.begin(); it != textures_.end();) {
    Texture& texture = it->second;
    const bool stale = texture.validated_frame == ~0ull || frame_ - texture.validated_frame > kTextureEvictFrames;
    // A texture loaded but never revalidated has validated_frame unset: give it the same time from its load.
    if (texture.validated_frame == ~0ull && texture.loaded) texture.validated_frame = frame_;
    if (stale && (texture.validated_frame != frame_ || !texture.loaded)) {
      if (texture.image.image) RetireImage(texture.image);
      it = textures_.erase(it);
      ++evicted;
    } else {
      ++it;
    }
  }
  stats_.textures_evicted += evicted;
}

void NativeCommandProcessor::BindTextures(Shader* vs, Shader* ps, uint32_t* shared) {
  static string::StringBuffer disassembly;
  // Per-frame memo of the descriptors: the same fetch constant (with the same sampler attributes of the fetch
  // instruction) is bound by many draws of a frame, and resolving it costs several hash map lookups (resolved
  // images, resolved copies, the R/B swap, the texture, its view, the sampler) - ~7 % of the GPU thread on the
  // console. Valid within one frame and until the next resolve (a resolve may replace the image behind an address);
  // a texture is revalidated against its guest pages at its first use in a frame, which is a memo miss.
  struct Memo {
    uint64_t key = 0, frame = UINT64_MAX, epoch = 0;
    uint32_t index = 0, heap = 0, sampler = 0;
    float inv_size[2] = {1.0f, 1.0f};
  };
  static Memo memo[1024];
  static const bool no_memo = std::getenv("SDNX_NO_TEXMEMO") != nullptr;  // A/B measurements
  uint32_t done = 0;
  for (Shader* shader : {vs, ps}) {
    if (!shader) continue;
    if (!shader->is_ucode_analyzed()) shader->AnalyzeUcode(disassembly);
    for (const Shader::TextureBinding& binding : shader->texture_bindings()) {
      uint32_t slot = binding.fetch_constant;
      // The shared block has 16 slots per dimension (slots 16+ would overlap the next dimension's indices).
      if (slot >= 16 || (done >> slot) & 1) continue;
      done |= 1u << slot;
      const auto& attr = binding.fetch_instr.attributes;
      struct {
        uint32_t words[6];
        uint32_t filters;
        float lod_bias;
      } key_data;
      std::memcpy(key_data.words, &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6],
                  sizeof(key_data.words));
      key_data.filters = uint32_t(attr.mag_filter) | uint32_t(attr.min_filter) << 8 | uint32_t(attr.mip_filter) << 16;
      key_data.lod_bias = attr.lod_bias;
      const uint64_t key = XXH3_64bits(&key_data, sizeof(key_data));
      Memo& m = memo[key & 1023];
      if (!no_memo && m.key == key && m.frame == frame_ && m.epoch == resolve_epoch_) {
        ++stats_.tex_memo_hits;
      } else {
        uint32_t heap = 0;
        float inv_size[2] = {1.0f, 1.0f};
        last_texture_format_ = VK_FORMAT_UNDEFINED;
        uint32_t index = GetTextureDescriptor(slot, heap, inv_size);
        xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(slot);
        uint32_t sampler = GetSamplerDescriptor(fetch, binding.fetch_instr);
        m.key = key;
        m.frame = frame_;
        m.epoch = resolve_epoch_;
        m.index = index;
        m.heap = heap;
        m.sampler = sampler;
        m.inv_size[0] = inv_size[0];
        m.inv_size[1] = inv_size[1];
      }
      shared[(m.heap * 64 + slot * 4) / 4] = m.index;
      shared[(192 + slot * 4) / 4] = m.sampler;
      std::memcpy(&shared[(360 + slot * 8) / 4], m.inv_size, 8);
    }
  }
}

}  // namespace rex::graphics::sdnx
