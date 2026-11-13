/**
 * Sleeping Dogs port (SDNX): native renderer command processor (stage 2b).
 *
 * The base CommandProcessor parses the PM4 ring. Here:
 *  - EDRAM-lite: host images per render target configuration (no format aliasing yet);
 *  - resolves (IssueCopy): blit of the resolve rectangle into a host "resolved texture" keyed by the physical
 *    destination address, then the clears the resolve requests;
 *  - swap: the front buffer (texture fetch constant 0, as Direct3D sets it) is copied by a compute shader into
 *    the presenter's guest output image;
 *  - draws: counted only (next step);
 *  - statistics every 5 s.
 * All work of a frame goes into one command buffer, submitted at the swap.
 */
#include <rex/graphics/sdnx/native_command_processor.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

#include "shaders/present_cs.h"

namespace rex::graphics::sdnx {

namespace {

VkFormat ColorTargetFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:  // 7e3, 16_16_16_16 (fixed and float)
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

VkFormat ResolveDestFormat(xenos::TextureFormat format, VkFormat source) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_2_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::TextureFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::TextureFormat::k_16_16:
      return VK_FORMAT_R16G16_UNORM;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::TextureFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    case xenos::TextureFormat::k_8:
      return VK_FORMAT_R8_UNORM;
    case xenos::TextureFormat::k_8_8:
      return VK_FORMAT_R8G8_UNORM;
    default:
      return source;
  }
}

// RB_COLOR_CLEAR is the packed value in the render target format.
VkClearColorValue DecodeColorClear(xenos::ColorRenderTargetFormat format, uint32_t lo, uint32_t hi) {
  VkClearColorValue c = {};
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      for (int i = 0; i < 4; ++i) c.float32[i] = float((lo >> (8 * i)) & 0xFF) / 255.0f;
      break;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      for (int i = 0; i < 3; ++i) c.float32[i] = float((lo >> (10 * i)) & 0x3FF) / 1023.0f;
      c.float32[3] = float(lo >> 30) / 3.0f;
      break;
    default:
      // Float formats: zero is by far the most common clear; exact decoding comes with EDRAM aliasing.
      (void)hi;
      break;
  }
  return c;
}

}  // namespace

void NativeCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= 0x2000 && index < RegisterFile::kRegisterCount) {
    register_file_->values[index] = value;
    return;
  }
  CommandProcessor::WriteRegister(index, value);
}

void NativeCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (start_index >= 0x2000 && start_index + num_registers <= RegisterFile::kRegisterCount) {
    uint32_t* dst = &register_file_->values[start_index];
    for (uint32_t i = 0; i < num_registers; ++i) dst[i] = __builtin_bswap32(base[i]);
    return;
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

NativeCommandProcessor::NativeCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

const ui::vulkan::VulkanDevice* NativeCommandProcessor::vulkan_device() const {
  return static_cast<const ui::vulkan::VulkanProvider*>(graphics_system_->provider())->vulkan_device();
}

uint64_t NativeCommandProcessor::RenderTargetKey(uint32_t base_tiles, uint32_t pitch_pixels, uint32_t format,
                                                 xenos::MsaaSamples msaa, bool is_depth) {
  // Key by the *host* format and the depth flag, not by the guest enum: the guest has aliases that map to
  // one host format (k_8_8_8_8 and k_8_8_8_8_GAMMA both to R8G8B8A8, 7e3 and 16_16_16_16 both to
  // R16G16B16A16_SFLOAT). Keying by the guest value made the draws and the resolve use two separate images
  // with the same address, so the resolve read an empty one and the frame stayed black.
  uint32_t host_format = is_depth ? 0x3Fu : uint32_t(ColorTargetFormat(xenos::ColorRenderTargetFormat(format)));
  // Fields: base 12 bits, pitch 16 bits, host format 8 bits (VkFormat values of the color formats used are
  // < 256), MSAA 2 bits, depth 1 bit - the format used to spill into the MSAA bits.
  return uint64_t(base_tiles & 0xFFF) | (uint64_t(pitch_pixels & 0xFFFF) << 12) |
         (uint64_t(host_format & 0xFF) << 28) | (uint64_t(msaa) << 36) | (uint64_t(is_depth) << 38);
}

bool NativeCommandProcessor::CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height,
                                         bool depth) {
  static const bool trace_image = std::getenv("SDNX_TRACE_DRAW") != nullptr;
  if (trace_image) {
    REXGPU_INFO("[sdnx_trace] create image {}x{} format {} depth={}", width, height, int(format), depth);
  }
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (trace_image) {
    REXGPU_INFO("[sdnx_trace]   vkCreateImage {}x{} format {} usage {:#x}", width, height, int(format), info.usage);
  }
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                        image.image, image.memory)) {
    REXGPU_ERROR("[sdnx_native] could not create a {}x{} image (format {})", width, height, int(format));
    return false;
  }
  image.format = format;
  image.width = width;
  image.height = height;
  image.aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
  image.initialized = false;
  VkImageViewCreateInfo view = {};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {
      depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
      0, 1, 0, 1};
  if (trace_image) {
    REXGPU_INFO("[sdnx_trace]   vkCreateImageView aspect={:#x}", uint32_t(view.subresourceRange.aspectMask));
  }
  device->functions().vkCreateImageView(device->device(), &view, nullptr, &image.view);
  if (trace_image) {
    REXGPU_INFO("[sdnx_trace]   image ready");
  }
  return true;
}

void NativeCommandProcessor::DestroyImage(Image& image) {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  if (image.view) dfn.vkDestroyImageView(device->device(), image.view, nullptr);
  if (image.image) dfn.vkDestroyImage(device->device(), image.image, nullptr);
  if (image.memory) dfn.vkFreeMemory(device->device(), image.memory, nullptr);
  image = Image();
}

void NativeCommandProcessor::InitializeLayout(Image& image) {
  if (image.initialized) return;
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {image.aspect, 0, 1, 0, 1};
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device()->functions();
  dfn.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
  // New memory has undefined contents: start colour at 0 and depth at 1 (far), as after a clear.
  VkImageSubresourceRange range = {image.aspect, 0, 1, 0, 1};
  if (image.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
    VkClearDepthStencilValue ds = {1.0f, 0};
    dfn.vkCmdClearDepthStencilImage(cb_, image.image, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &range);
  } else {
    VkClearColorValue color = {};
    dfn.vkCmdClearColorImage(cb_, image.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
  }
  FullBarrier();
  image.initialized = true;
}

void NativeCommandProcessor::RetireImage(Image& image) {
  if (image.image) {
    // Into the slot of the frame being recorded: open it first, otherwise the image would land in the slot
    // EnsureFrame empties right away while the previous frame may still be using it.
    EnsureFrame();
    RetireViews(image.image);
    retired_images_[uint32_t(frame_ % kFramesInFlight)].push_back(image);
  }
  image = Image();
}

void NativeCommandProcessor::FullBarrier() {
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vulkan_device()->functions().vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr,
                                                    0, nullptr);
}

NativeCommandProcessor::Image* NativeCommandProcessor::GetRenderTarget(uint32_t base_tiles, uint32_t pitch_pixels,
                                                                       uint32_t format, xenos::MsaaSamples msaa,
                                                                       bool is_depth, uint32_t min_height) {
  // Images are in EDRAM samples: 2X MSAA doubles the height, 4X both dimensions. D3D clears at half the
  // resolution with 4X MSAA (pitch 640 for a 1280 surface) - the same EDRAM tiles, so the same image; keyed by
  // the pixel pitch and MSAA, those clears went to an image of their own and the UI stencil was never cleared.
  uint32_t sx = uint32_t(msaa >= xenos::MsaaSamples::k4X), sy = uint32_t(msaa >= xenos::MsaaSamples::k2X);
  pitch_pixels = std::max(pitch_pixels << sx, uint32_t(8));
  min_height = std::max(min_height << sy, uint32_t(8));
  uint64_t key = RenderTargetKey(base_tiles, pitch_pixels, format, xenos::MsaaSamples::k1X, is_depth);
  Image& image = render_targets_[key];
  if (image.image && image.height >= min_height) {
    return &image;
  }
  uint32_t height = std::max(min_height, image.height);
  height = (height + 15) & ~15u;
  if (image.image) {
    // Grow: the old contents are dropped (rare; the next stage keeps them). The image may still be used by
    // commands recorded in this frame, so it is destroyed later.
    RetireImage(image);
  }
  // Creating the image records an initialization barrier, and barriers must not be recorded inside a
  // render pass; also, after a submit cb_ is null until the next frame is opened, so make sure a command
  // buffer is recording first (recording into the null handle crashed the NVIDIA driver).
  EnsureFrame();
  EndRendering();
  VkFormat vk_format =
      is_depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : ColorTargetFormat(xenos::ColorRenderTargetFormat(format));
  static const bool trace_rt = std::getenv("SDNX_TRACE_DRAW") != nullptr;
  if (trace_rt) {
    REXGPU_INFO("[sdnx_trace] render target base={} pitch={} guest_format={} depth={} height={} -> {}", base_tiles,
                pitch_pixels, format, is_depth, height, int(vk_format));
  }
  if (!CreateImage(image, vk_format, pitch_pixels, height, is_depth)) {
    render_targets_.erase(key);
    return nullptr;
  }
  InitializeLayout(image);
  return &image;
}

NativeCommandProcessor::Image* NativeCommandProcessor::GetResolved(uint32_t address, VkFormat format,
                                                                   uint32_t width, uint32_t height) {
  width = std::max(width, uint32_t(8));
  height = std::max(height, uint32_t(8));
  Image& image = resolved_[address];
  if (image.image && image.format == format && image.width >= width && image.height >= height) {
    return &image;
  }
  if (image.image) RetireImage(image);
  EnsureFrame();
  EndRendering();
  if (!CreateImage(image, format, width, height, false)) {
    resolved_.erase(address);
    return nullptr;
  }
  InitializeLayout(image);
  return &image;
}

bool NativeCommandProcessor::CreatePresentPipeline() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkDescriptorSetLayoutBinding bindings[3] = {};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[1] = {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info = {};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_info.bindingCount = 3;
  set_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device->device(), &set_info, nullptr, &present_set_layout_) != VK_SUCCESS)
    return false;
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo layout_info = {};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &present_set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (dfn.vkCreatePipelineLayout(device->device(), &layout_info, nullptr, &present_pipeline_layout_) != VK_SUCCESS)
    return false;
  present_pipeline_ = ui::vulkan::util::CreateComputePipeline(device, present_pipeline_layout_, shaders::kPresentCs,
                                                              sizeof(shaders::kPresentCs));
  if (present_pipeline_ == VK_NULL_HANDLE) return false;
  VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kFramesInFlight},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, kFramesInFlight},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight}};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = kFramesInFlight;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(device->device(), &pool_info, nullptr, &present_descriptor_pool_) != VK_SUCCESS)
    return false;
  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
  layouts.fill(present_set_layout_);
  VkDescriptorSetAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = present_descriptor_pool_;
  alloc.descriptorSetCount = kFramesInFlight;
  alloc.pSetLayouts = layouts.data();
  if (dfn.vkAllocateDescriptorSets(device->device(), &alloc, present_sets_.data()) != VK_SUCCESS) return false;
  VkSamplerCreateInfo sampler = {};
  sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.maxLod = 0.0f;
  return dfn.vkCreateSampler(device->device(), &sampler, nullptr, &linear_sampler_) == VK_SUCCESS;
}

bool NativeCommandProcessor::SetupContext() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device->device(), &pool_info, nullptr, &command_pool_) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = kFramesInFlight;
  if (dfn.vkAllocateCommandBuffers(device->device(), &alloc_info, command_buffers_.data()) != VK_SUCCESS)
    return false;
  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (VkFence& fence : fences_) {
    if (dfn.vkCreateFence(device->device(), &fence_info, nullptr, &fence) != VK_SUCCESS) return false;
  }
  if (!CreatePresentPipeline()) {
    REXGPU_ERROR("[sdnx_native] could not create the output pipeline");
    return false;
  }
  if (!InitializeDraws()) {
    REXGPU_ERROR("[sdnx_native] could not create the draw resources");
    return false;
  }
  stats_start_ = std::chrono::steady_clock::now();
  REXGPU_INFO("[sdnx_native] context ready ({} frames in flight, EDRAM-lite, resolves, output)", kFramesInFlight);
  return true;
}

void NativeCommandProcessor::ShutdownContext() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  dfn.vkDeviceWaitIdle(device->device());
  ShutdownQueries();
  ShutdownGeometry();
  ShutdownDraws();
  for (auto& slot_images : retired_images_) {
    for (Image& retired : slot_images) DestroyImage(retired);
    slot_images.clear();
  }
  for (auto& it : render_targets_) DestroyImage(it.second);
  render_targets_.clear();
  DestroyImage(depth_resolve_temp_);
  for (auto& it : resolved_stencil_) DestroyImage(it.second);
  resolved_stencil_.clear();
  for (auto& it : resolved_) DestroyImage(it.second);
  resolved_.clear();
  if (linear_sampler_) dfn.vkDestroySampler(device->device(), linear_sampler_, nullptr);
  if (present_descriptor_pool_) dfn.vkDestroyDescriptorPool(device->device(), present_descriptor_pool_, nullptr);
  if (present_pipeline_) dfn.vkDestroyPipeline(device->device(), present_pipeline_, nullptr);
  if (present_pipeline_layout_) dfn.vkDestroyPipelineLayout(device->device(), present_pipeline_layout_, nullptr);
  if (present_set_layout_) dfn.vkDestroyDescriptorSetLayout(device->device(), present_set_layout_, nullptr);
  for (VkFence& fence : fences_) {
    if (fence) dfn.vkDestroyFence(device->device(), fence, nullptr);
    fence = VK_NULL_HANDLE;
  }
  if (command_pool_) dfn.vkDestroyCommandPool(device->device(), command_pool_, nullptr);
  command_pool_ = VK_NULL_HANDLE;
  shaders_.clear();
}

void NativeCommandProcessor::EnsureFrame() {
  if (frame_open_) return;
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  uint32_t slot = uint32_t(frame_ % kFramesInFlight);
  dfn.vkWaitForFences(device->device(), 1, &fences_[slot], VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(device->device(), 1, &fences_[slot]);
  cb_ = command_buffers_[slot];
  VkCommandBufferBeginInfo begin = {};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(cb_, &begin);  // resets it (pool created with RESET_COMMAND_BUFFER_BIT)
  bound_ = {};  // nothing is bound in a new command buffer
  upload_[slot].offset = 0;
  frame_open_ = true;
  ResetQueries(slot);  // the slot's previous frame has completed: its occlusion counts are readable
  ReleaseRetiredGeometry(slot);
  callback_frame_.store(uint32_t(frame_) + 1, std::memory_order_relaxed);
  ReleaseRetiredViews(slot);
  for (Image& retired : retired_images_[slot]) DestroyImage(retired);
  retired_images_[slot].clear();
  if (!dummies_initialized_) {
    // The placeholder textures: GENERAL layout (as their descriptors say) and white. They were left in
    // UNDEFINED, so the shaders sampled undefined data (zero on NVIDIA): textured UI came out black.
    dummies_initialized_ = true;
    for (Image* dummy : {&dummy_2d_, &dummy_3d_, &dummy_cube_}) {
      uint32_t layers = dummy == &dummy_cube_ ? 6 : 1;
      VkImageMemoryBarrier barrier = {};
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = dummy->image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      dfn.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                               0, nullptr, 1, &barrier);
      VkClearColorValue white = {{1.0f, 1.0f, 1.0f, 1.0f}};
      dfn.vkCmdClearColorImage(cb_, dummy->image, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &barrier.subresourceRange);
      dummy->initialized = true;
    }
    FullBarrier();
  }
}

bool NativeCommandProcessor::SubmitFrame() {
  if (!frame_open_) return true;
  EndRendering();
  CopyQueryResults(uint32_t(frame_ % kFramesInFlight));
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  dfn.vkEndCommandBuffer(cb_);
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb_;
  VkResult result;
  {
    auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    result = dfn.vkQueueSubmit(queue.queue(), 1, &submit, fences_[uint32_t(frame_ % kFramesInFlight)]);
  }
  frame_open_ = false;
  cb_ = VK_NULL_HANDLE;
  ++frame_;
  return result == VK_SUCCESS;
}

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  ++stats_.shader_loads;
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) return it->second.get();
  ++stats_.shader_new;
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info, bool major_mode_explicit) {
  (void)prim_type;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  // A resolve arrives as a draw in the EDRAM copy mode (as in the Vulkan backend).
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++stats_.draws;
  stats_.indices += index_count;
  return DrawNative(prim_type, index_count, index_buffer_info);
}

bool NativeCommandProcessor::IssueCopy() {
  ++stats_.copies;
  static const bool skip_resolves = std::getenv("SDNX_NO_RESOLVE") != nullptr;
  if (skip_resolves) {
    return true;
  }
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(*register_file_, *memory_, 1, 1, false, false, info)) {
    return true;
  }
  if (!info.coordinate_info.width_div_8 || !info.height_div_8) {
    return true;
  }
  EnsureFrame();
  EndRendering();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device()->functions();
  const RegisterFile& regs = *register_file_;
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface.surface_pitch;
  xenos::MsaaSamples msaa = surface.msaa_samples;
  bool copy_depth = info.IsCopyingDepth();

  // The rectangle in surface pixels, from the packed EDRAM base offset (see draw_util::GetResolveInfo).
  const draw_util::ResolveEdramInfo& edram = copy_depth ? info.depth_edram_info : info.color_edram_info;
  uint32_t original_base = copy_depth ? info.depth_original_base : info.color_original_base;
  uint32_t sx = uint32_t(msaa >= xenos::MsaaSamples::k4X), sy = uint32_t(msaa >= xenos::MsaaSamples::k2X);
  uint32_t pitch_tiles = std::max(xenos::GetSurfacePitchTiles(pitch, msaa, false), 1u);
  uint32_t offset_tiles = (edram.base_tiles - original_base) >> (copy_depth ? 0 : edram.format_is_64bpp);
  uint32_t x0 = ((offset_tiles % pitch_tiles) * xenos::kEdramTileWidthSamples +
                 (info.coordinate_info.edram_offset_x_div_8 << (sx + 3))) >> sx;
  uint32_t y0 = ((offset_tiles / pitch_tiles) * xenos::kEdramTileHeightSamples +
                 (info.coordinate_info.edram_offset_y_div_8 << (sy + 3))) >> sy;
  uint32_t width = info.coordinate_info.width_div_8 * 8, height = info.height_div_8 * 8;

  uint32_t format =
      copy_depth ? uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format) : uint32_t(edram.format);
  Image* source = GetRenderTarget(original_base, pitch, format, msaa, copy_depth, y0 + height);

  // Copy.
  if (source && info.copy_dest_extent_length) {
    if (copy_depth) {
      ++stats_.copies_depth;
      ResolveDepth(*source, info, x0, y0, width, height, sx, sy);
    } else {
      uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
      uint32_t dest_w = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
      uint32_t dest_h = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
      VkFormat dest_format =
          ResolveDestFormat(xenos::TextureFormat(info.copy_dest_info.copy_dest_format), source->format);
      // The guest asks for a different format here (HDR 7e3 -> 16_16_16_16), but vkCmdBlitImage only
      // accepts blit-compatible formats. Allocate the resolved texture in the source format so the copy is
      // legal; the value encoding of the guest format is a separate conversion step (compute shader).
      // The rectangle lands at the destination offset (not at its render target position).
      uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
      uint32_t dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
      Image* dest =
          GetResolved(dest_base, source->format, std::max(dest_w, dx + width), std::max(dest_h, dy + height));
      resolved_extent_[dest_base] = info.copy_dest_extent_start + info.copy_dest_extent_length - dest_base;
      if (log_frame_) {
        REXGPU_INFO("[sdnx_frame] resolve rt base={} fmt={} {}x{} at {},{} -> {:#x} at {},{} fmt={} swap={} clear "
                    "color={} depth={}",
                    original_base, format, width, height, x0, y0, dest_base, dx, dy,
                    uint32_t(info.copy_dest_info.copy_dest_format), uint32_t(info.copy_dest_info.copy_dest_swap),
                    uint32_t(info.rb_copy_control.color_clear_enable), uint32_t(info.rb_copy_control.depth_clear_enable));
      }
      static const bool trace_resolve = std::getenv("SDNX_TRACE_RESOLVE") != nullptr;
      static std::unordered_map<uint64_t, int> traced_resolves;
      uint64_t trace_key = uint64_t(dest_base) ^ (uint64_t(format) << 32) ^ (uint64_t(width) << 40) ^
                           (uint64_t(uint32_t(info.copy_dest_info.value)) << 20);
      if (trace_resolve && traced_resolves[trace_key]++ == 0) {
        REXGPU_INFO("[sdnx_resolve] rt base={} fmt={} host={} {}x{} at {},{} -> dest {:#x} fmt={} number={} "
                    "exp_bias={} dest {}x{} img {}x{} dest offset {},{}",
                    original_base, format, int(source->format), width, height, x0, y0, dest_base,
                    uint32_t(info.copy_dest_info.copy_dest_format), uint32_t(info.copy_dest_info.copy_dest_number),
                    int32_t(info.copy_dest_info.copy_dest_exp_bias), dest_w, dest_h, dest ? dest->width : 0,
                    dest ? dest->height : 0, dx, dy);
      }
      (void)dest_format;
      ++resolved_generation_[dest_base];
      resolved_swap_[dest_base] = info.copy_dest_info.copy_dest_swap != 0;
      resolved_frame_[dest_base] = frame_;
      if (dest) {
        // vkCmdBlitImage needs blit-compatible formats; the HDR 7e3 -> 16F resolve is not, and an
        // incompatible blit is invalid usage (it took the NVIDIA driver down). Skip just the copy and count
        // it; the compute-shader conversion is the proper fix (next stage).
        static const bool trace_copy = std::getenv("SDNX_TRACE_DRAW") != nullptr;
        if (source->format != dest->format) {
          ++stats_.copies_incompatible;
          if (trace_copy) {
            REXGPU_INFO("[sdnx_trace] resolve skipped: {} -> {} ({}x{} at {},{}), dest base {:#x}",
                        int(source->format), int(dest->format), width, height, x0, y0, dest_base);
          }
        } else {
          FullBarrier();
          if (trace_copy) {
            REXGPU_INFO("[sdnx_trace] resolve src img={} base={:#x} fmt={} pitch={} msaa={} drawn={} -> dest={:#x} "
                        "{}x{}",
                        reinterpret_cast<uintptr_t>(source), original_base, int(source->format), pitch,
                        uint32_t(msaa), uint32_t(source->drawn), dest_base, width, height);
          }
          VkImageBlit blit = {};
          blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          // The source is in samples (MSAA surfaces are larger): the blit downsamples.
          blit.srcOffsets[0] = {int32_t(x0 << sx), int32_t(y0 << sy), 0};
          blit.srcOffsets[1] = {int32_t(std::min((x0 + width) << sx, source->width)),
                                int32_t(std::min((y0 + height) << sy, source->height)), 1};
          blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          blit.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
          uint32_t copy_w = uint32_t(blit.srcOffsets[1].x - blit.srcOffsets[0].x) >> sx;
          uint32_t copy_h = uint32_t(blit.srcOffsets[1].y - blit.srcOffsets[0].y) >> sy;
          copy_w = std::min(copy_w, dest->width > dx ? dest->width - dx : 0u);
          copy_h = std::min(copy_h, dest->height > dy ? dest->height - dy : 0u);
          blit.dstOffsets[1] = {int32_t(dx + copy_w), int32_t(dy + copy_h), 1};
          blit.srcOffsets[1].x = blit.srcOffsets[0].x + int32_t(copy_w << sx);
          blit.srcOffsets[1].y = blit.srcOffsets[0].y + int32_t(copy_h << sy);
          if (copy_w && copy_h) {
            dfn.vkCmdBlitImage(cb_, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_GENERAL,
                               1, &blit, (sx || sy) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
          }
        }
        DumpResolveIfRequested(dest_base, *dest);
      }
    }
  }

  // Clears requested by the resolve.
  auto copy_control = info.rb_copy_control;
  if (copy_control.color_clear_enable && !copy_depth) {
    Image* target = GetRenderTarget(info.color_original_base, pitch, uint32_t(info.color_edram_info.format), msaa,
                                    false, y0 + height);
    if (target) {
      FullBarrier();
      VkClearColorValue color = DecodeColorClear(xenos::ColorRenderTargetFormat(info.color_edram_info.format),
                                                 info.rb_color_clear, info.rb_color_clear_lo);
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      // vkCmdClearColorImage clears the whole image; the resolve rectangle is the whole target in practice.
      // EDRAM is one memory: a clear through one format also clears the surfaces of other formats at the same
      // base and pitch. Images are per format here, so clear all of them - otherwise what was drawn through
      // another format stays (the menu selection highlight accumulated on every item it passed over).
      uint64_t base_pitch = uint64_t(info.color_original_base & 0xFFF) | (uint64_t((pitch << sx) & 0xFFFF) << 12);
      for (auto& it : render_targets_) {
        bool is_depth = (it.first >> 38) & 1;
        if (is_depth || (it.first & 0xFFFFFFF) != base_pitch || !it.second.image) continue;
        dfn.vkCmdClearColorImage(cb_, it.second.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        ++stats_.clears;
      }
    }
  }
  if (copy_control.depth_clear_enable) {
    Image* target = GetRenderTarget(info.depth_original_base, pitch,
                                    uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format), msaa, true, y0 + height);
    if (target) {
      FullBarrier();
      VkClearDepthStencilValue ds = {float(info.rb_depth_clear >> 8) / float(0xFFFFFF),
                                     info.rb_depth_clear & 0xFF};
      VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
      dfn.vkCmdClearDepthStencilImage(cb_, target->image, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &range);
      ++stats_.clears;
    }
  }
  return true;
}

void NativeCommandProcessor::ResolveDepth(const Image& source, const draw_util::ResolveInfo& info, uint32_t x0,
                                          uint32_t y0, uint32_t width, uint32_t height, uint32_t sx,
                                          uint32_t sy) {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  uint32_t dest_base = (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
  uint32_t dest_w = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
  uint32_t dest_h = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
  uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8;
  uint32_t dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
  Image* dest = GetResolved(dest_base, VK_FORMAT_R32_SFLOAT, std::max(dest_w, dx + width),
                            std::max(dest_h, dy + height));
  ++resolved_generation_[dest_base];
  resolved_frame_[dest_base] = frame_;
  resolved_extent_[dest_base] = info.copy_dest_extent_start + info.copy_dest_extent_length - dest_base;
  if (log_frame_) {
    REXGPU_INFO("[sdnx_frame] depth resolve {}x{} at {},{} (samples x{} y{}) -> {:#x} at {},{}", width, height, x0,
                y0, 1u << sx, 1u << sy, dest_base, dx, dy);
  }
  if (!dest) return;
  // Source rectangle in samples, clamped to the image.
  uint32_t src_x = x0 << sx, src_y = y0 << sy;
  if (src_x >= source.width || src_y >= source.height) return;
  uint32_t src_w = std::min(width << sx, source.width - src_x), src_h = std::min(height << sy, source.height - src_y);
  uint32_t out_w = std::min(src_w >> sx, dest->width > dx ? dest->width - dx : 0u);
  uint32_t out_h = std::min(src_h >> sy, dest->height > dy ? dest->height - dy : 0u);
  if (!out_w || !out_h) return;
  src_w = out_w << sx;
  src_h = out_h << sy;
  VkDeviceSize offset = Upload(nullptr, VkDeviceSize(src_w) * src_h * 4, 16);
  if (offset == UINT64_MAX) return;
  VkBuffer buffer = upload_[uint32_t(frame_ % kFramesInFlight)].buffer;
  EndRendering();
  FullBarrier();
  VkBufferImageCopy region = {};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(src_x), int32_t(src_y), 0};
  region.imageExtent = {src_w, src_h, 1};
  dfn.vkCmdCopyImageToBuffer(cb_, source.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
  FullBarrier();
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  if (!sx && !sy) {
    region.imageOffset = {int32_t(dx), int32_t(dy), 0};
    dfn.vkCmdCopyBufferToImage(cb_, buffer, dest->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    // Stencil, for TryRestoreDepth.
    Image& stencil = resolved_stencil_[dest_base];
    if (stencil.image && (stencil.width < dest->width || stencil.height < dest->height)) RetireImage(stencil);
    if (!stencil.image && CreateImage(stencil, VK_FORMAT_R8_UNORM, dest->width, dest->height, false)) {
      InitializeLayout(stencil);
    }
    VkDeviceSize stencil_offset = stencil.image ? Upload(nullptr, VkDeviceSize(src_w) * src_h, 16) : UINT64_MAX;
    if (stencil_offset != UINT64_MAX) {
      FullBarrier();
      VkBufferImageCopy s_region = {};
      s_region.bufferOffset = stencil_offset;
      s_region.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
      s_region.imageOffset = {int32_t(src_x), int32_t(src_y), 0};
      s_region.imageExtent = {src_w, src_h, 1};
      dfn.vkCmdCopyImageToBuffer(cb_, source.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &s_region);
      FullBarrier();
      s_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      s_region.imageOffset = {int32_t(dx), int32_t(dy), 0};
      dfn.vkCmdCopyBufferToImage(cb_, buffer, stencil.image, VK_IMAGE_LAYOUT_GENERAL, 1, &s_region);
    }
  } else {
    if (!depth_resolve_temp_.image || depth_resolve_temp_.width < src_w || depth_resolve_temp_.height < src_h) {
      uint32_t tw = std::max(src_w, depth_resolve_temp_.width), th = std::max(src_h, depth_resolve_temp_.height);
      if (depth_resolve_temp_.image) RetireImage(depth_resolve_temp_);
      if (!CreateImage(depth_resolve_temp_, VK_FORMAT_R32_SFLOAT, tw, th, false)) return;
      InitializeLayout(depth_resolve_temp_);
    }
    region.imageOffset = {0, 0, 0};
    dfn.vkCmdCopyBufferToImage(cb_, buffer, depth_resolve_temp_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    FullBarrier();
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(src_w), int32_t(src_h), 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
    blit.dstOffsets[1] = {int32_t(dx + out_w), int32_t(dy + out_h), 1};
    dfn.vkCmdBlitImage(cb_, depth_resolve_temp_.image, VK_IMAGE_LAYOUT_GENERAL, dest->image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_NEAREST);
  }
  FullBarrier();
}

bool NativeCommandProcessor::TryRestoreDepth(Shader* ps) {
  const RegisterFile& regs = *register_file_;
  if (!ps || regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) return false;
  if ((regs[XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF) != 0xF) return false;
  auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  if (depth_control.z_enable || depth_control.stencil_enable) return false;
  auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
  if (color_info.color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8) return false;
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  if (surface.msaa_samples != xenos::MsaaSamples::k1X || !surface.surface_pitch) return false;
  static string::StringBuffer disassembly;
  if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(disassembly);
  if (ps->texture_bindings().size() != 1) return false;
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(ps->texture_bindings()[0].fetch_constant);
  if (fetch.type != xenos::FetchConstantType::kTexture || fetch.format != xenos::TextureFormat::k_8_8_8_8) return false;
  uint32_t base = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFF;
  auto first = resolved_.find(base);
  if (first == resolved_.end() || first->second.format != VK_FORMAT_R32_SFLOAT) return false;
  // The colour target must alias a depth buffer drawn to before (same EDRAM base and pitch).
  uint32_t pitch = surface.surface_pitch;
  if (render_targets_.find(RenderTargetKey(color_info.color_base, std::max(pitch, 8u), 0, xenos::MsaaSamples::k1X,
                                           true)) == render_targets_.end()) {
    return false;
  }
  uint32_t tw = fetch.size_2d.width + 1, th = fetch.size_2d.height + 1;
  Image* target = GetRenderTarget(color_info.color_base, pitch, 0, xenos::MsaaSamples::k1X, true, th);
  if (!target) return false;
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device()->functions();
  EnsureFrame();
  EndRendering();
  VkBuffer buffer = upload_[uint32_t(frame_ % kFramesInFlight)].buffer;
  uint32_t pitch_pixels = fetch.pitch ? uint32_t(fetch.pitch) << 5 : ((tw + 31) & ~31u);
  uint64_t row_bytes = uint64_t(pitch_pixels) * 4;
  for (const auto& part : resolved_) {
    if (!part.second.image || part.second.format != VK_FORMAT_R32_SFLOAT) continue;
    if (part.first < base || part.first >= base + row_bytes * th) continue;
    uint64_t delta = part.first - base;
    uint32_t y = uint32_t(delta / row_bytes), x = uint32_t((delta % row_bytes) / 4);
    if (x >= tw || y >= th || x >= target->width || y >= target->height) continue;
    uint32_t w = std::min({tw - x, part.second.width, target->width - x});
    uint32_t h = std::min({th - y, part.second.height, target->height - y});
    VkDeviceSize offset = Upload(nullptr, VkDeviceSize(w) * h * 4, 16);
    if (offset == UINT64_MAX) continue;
    FullBarrier();
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    dfn.vkCmdCopyImageToBuffer(cb_, part.second.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    FullBarrier();
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    dfn.vkCmdCopyBufferToImage(cb_, buffer, target->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    auto stencil = resolved_stencil_.find(part.first);
    if (stencil == resolved_stencil_.end() || !stencil->second.image) continue;
    w = std::min(w, stencil->second.width);
    h = std::min(h, stencil->second.height);
    offset = Upload(nullptr, VkDeviceSize(w) * h, 16);
    if (offset == UINT64_MAX) continue;
    region.bufferOffset = offset;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    dfn.vkCmdCopyImageToBuffer(cb_, stencil->second.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
    FullBarrier();
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    region.imageOffset = {int32_t(x), int32_t(y), 0};
    dfn.vkCmdCopyBufferToImage(cb_, buffer, target->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  }
  FullBarrier();
  if (log_frame_) {
    REXGPU_INFO("[sdnx_frame] depth restore from {:#x} ({}x{}) to depth base {} pitch {}", base, tw, th,
                uint32_t(color_info.color_base), pitch);
  }
  return true;
}

void NativeCommandProcessor::ReportStats() {
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - stats_start_).count();
  if (seconds < 5.0) return;
  double frames = std::max<double>(1.0, double(stats_.swaps));
  REXGPU_INFO(
      "[sdnx_native] {:.1f} s: {:.1f} swaps/s, {:.0f} draws/frame ({:.0f} indices/frame), {:.1f} resolves/frame "
      "({:.1f} depth), {:.1f} clears/frame, front buffer found {:.0f}%, shader loads {:.0f}/frame, new shaders {}, "
      "total shaders {}, render targets {}, resolved textures {}; drawn {:.0f}/frame (pulled {:.0f}), missing vs "
      "{:.0f} ps {:.0f}, "
      "skipped {:.0f}, new pipelines {}; geometry: hits {:.0f}/frame, uploads {:.1f}/frame ({:.2f} MB/frame), "
      "dynamic {:.0f}/frame, direct copies {:.2f} MB/frame, cached {}; upload buffer full {:.0f}/frame; geo first {:.0f} hot {:.0f} flagged {:.0f} changed {:.1f} /frame, write "
      "callbacks {:.0f}/frame; texture loads {:.1f}/frame ({:.2f} MB/frame)",
      seconds, stats_.swaps / seconds, stats_.draws / frames, stats_.indices / frames, stats_.copies / frames,
      stats_.copies_depth / frames, stats_.clears / frames, 100.0 * stats_.front_found / frames,
      stats_.shader_loads / frames, stats_.shader_new, shaders_.size(), render_targets_.size(), resolved_.size(),
      stats_.drawn / frames, stats_.pulled / frames, stats_.missing_vs / frames, stats_.missing_ps / frames, stats_.skipped / frames,
      stats_.new_pipelines, stats_.geo_hits / frames, stats_.geo_uploads / frames,
      stats_.geo_bytes / frames / 1048576.0, stats_.geo_dynamic / frames, stats_.geo_direct_bytes / frames / 1048576.0,
      geometry_.size(), stats_.upload_full / frames, stats_.geo_first / frames, stats_.geo_hot / frames,
      stats_.geo_flagged / frames, stats_.geo_changed / frames,
      double(write_callbacks_.load() - write_callbacks_reported_) / frames, stats_.tex_loads / frames,
      stats_.tex_bytes / frames / 1048576.0);
  write_callbacks_reported_ = write_callbacks_.load();
  if (stats_.prof_draws) {
    // steady_clock ticks -> microseconds per drawn draw and per frame.
    using period = std::chrono::steady_clock::period;
    const double to_us = 1e6 * double(period::num) / double(period::den);
    const double d = double(stats_.prof_draws);
    double total = 0;
    for (uint64_t ns : stats_.prof_ns) total += double(ns);
    REXGPU_INFO("[sdnx_prof] per draw (us): state {:.2f} vertices {:.2f} (pairing {:.2f} pulled {:.2f} bindings {:.2f}) indices {:.2f} constants+textures {:.2f} "
                "pipeline {:.2f} record {:.2f} | total {:.2f} us/draw, {:.2f} ms/frame in draws | hashed geometry: "
                "hits {:.0f} (reused {:.0f}) fills {:.0f} ({:.2f} MB) /frame, entries {}, hashed {:.1f} MB/frame of bindings {:.1f} MB; hot refusals {:.0f}/frame, page age {:.0f} frames, strikes avg {:.1f} max {}; constants reused {:.0f}/frame, set binds skipped {:.0f}/frame",
                stats_.prof_ns[0] * to_us / d, stats_.prof_ns[1] * to_us / d, stats_.prof_sub[0] * to_us / d,
                stats_.prof_sub[1] * to_us / d, stats_.prof_sub[2] * to_us / d, stats_.prof_ns[2] * to_us / d,
                stats_.prof_ns[3] * to_us / d, stats_.prof_ns[4] * to_us / d, stats_.prof_ns[5] * to_us / d,
                total * to_us / d, total * to_us / 1000.0 / frames, stats_.hash_hits / frames, stats_.hash_reuse / frames,
                stats_.hash_fills / frames, stats_.hash_fill_bytes / frames / 1048576.0, hashed_.size(),
                stats_.hash_bytes / frames / 1048576.0, stats_.hash_whole_bytes / frames / 1048576.0, stats_.hot_checks / frames,
                stats_.hot_checks ? double(stats_.hot_age) / stats_.hot_checks : 0.0,
                stats_.hot_checks ? double(stats_.hot_strikes) / stats_.hot_checks : 0.0, stats_.hot_strikes_max,
                stats_.constants_reused / frames, stats_.constants_bind_skipped / frames);
  }
  stats_ = {};
  stats_start_ = now;
}

void NativeCommandProcessor::BeginFrontDump(const Image& front) {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  uint32_t bpp = (front.format == VK_FORMAT_R16G16B16A16_UNORM || front.format == VK_FORMAT_R16G16B16A16_SFLOAT)
                     ? 8u
                     : 4u;
  VkDeviceSize size = VkDeviceSize(front.width) * front.height * bpp;
  if (!dump_buffer_ || dump_size_ < size) {
    if (dump_buffer_) {
      dfn.vkUnmapMemory(device->device(), dump_memory_);
      dfn.vkDestroyBuffer(device->device(), dump_buffer_, nullptr);
      dfn.vkFreeMemory(device->device(), dump_memory_, nullptr);
      dump_buffer_ = VK_NULL_HANDLE;
      dump_memory_ = VK_NULL_HANDLE;
      dump_mapping_ = nullptr;
    }
    uint32_t memory_type = 0;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, ui::vulkan::util::MemoryPurpose::kReadback, dump_buffer_,
            dump_memory_, &memory_type)) {
      REXGPU_ERROR("[sdnx_front] could not create the readback buffer");
      dump_remaining_ = 0;
      return;
    }
    dfn.vkMapMemory(device->device(), dump_memory_, 0, VK_WHOLE_SIZE, 0, &dump_mapping_);
    dump_size_ = size;
  }
  dump_address_ = front_address_;
  dump_width_ = front.width;
  dump_height_ = front.height;
  dump_bpp_ = bpp;
  FullBarrier();
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {front.width, front.height, 1};
  dfn.vkCmdCopyImageToBuffer(cb_, front.image, VK_IMAGE_LAYOUT_GENERAL, dump_buffer_, 1, &region);
  dump_pending_ = true;
}

void NativeCommandProcessor::DumpResolveIfRequested(uint32_t dest_base, const Image& dest) {
  static const uint32_t dump_addr =
      std::getenv("SDNX_DUMP_ADDR") ? uint32_t(std::strtoul(std::getenv("SDNX_DUMP_ADDR"), nullptr, 16)) : 0;
  static const uint32_t nth = std::getenv("SDNX_DUMP_NTH") ? uint32_t(std::atoi(std::getenv("SDNX_DUMP_NTH"))) : 0;
  if (!dump_nth_frame_ || !nth || dest_base != dump_addr || ++dump_nth_count_ != nth) return;
  dump_nth_frame_ = false;
  if (dump_pending_ || dump_remaining_ <= 0) return;
  front_address_ = dest_base;
  BeginFrontDump(dest);
}

void NativeCommandProcessor::FinishFrontDump() {
  if (!dump_pending_) return;
  dump_pending_ = false;
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  dfn.vkDeviceWaitIdle(device->device());
  const uint8_t* data = static_cast<const uint8_t*>(dump_mapping_);
  if (!data) return;
  double sum[3] = {0, 0, 0};
  double maxv[3] = {0, 0, 0};
  uint64_t nonblack = 0;
  uint64_t count = uint64_t(dump_width_) * dump_height_;
  std::string ppm;
  // The image of the last requested swap is written (the first one is usually a boot screen).
  const bool write_ppm = dump_remaining_ == 1;
  if (write_ppm) ppm.reserve(size_t(count) * 3 + 64);
  for (uint64_t i = 0; i < count; ++i) {
    float rgb[3] = {0, 0, 0};
    static const bool dump_alpha = std::getenv("SDNX_DUMP_ALPHA") != nullptr;  // alpha as grey
    if (dump_bpp_ == 4) {
      uint32_t v = *reinterpret_cast<const uint32_t*>(data + i * 4);
      rgb[0] = float(v & 0xFF) / 255.0f;
      rgb[1] = float((v >> 8) & 0xFF) / 255.0f;
      rgb[2] = float((v >> 16) & 0xFF) / 255.0f;
      if (dump_alpha) rgb[0] = rgb[1] = rgb[2] = float(v >> 24) / 255.0f;
    } else {
      const uint16_t* h = reinterpret_cast<const uint16_t*>(data + i * 8);
      auto half_to_float = [](uint16_t hh) {
        uint32_t sign = (hh >> 15) & 1, exp = (hh >> 10) & 0x1F, frac = hh & 0x3FF;
        float v;
        if (exp == 0) {
          v = frac ? std::ldexp(float(frac) / 1024.0f, -14) : 0.0f;
        } else if (exp == 31) {
          v = frac ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        } else {
          v = std::ldexp(1.0f + float(frac) / 1024.0f, int(exp) - 15);
        }
        return sign ? -v : v;
      };
      rgb[0] = half_to_float(h[0]);
      rgb[1] = half_to_float(h[1]);
      rgb[2] = half_to_float(h[2]);
    }
    for (int c = 0; c < 3; ++c) {
      sum[c] += rgb[c];
      if (rgb[c] > maxv[c]) maxv[c] = rgb[c];
    }
    if (rgb[0] + rgb[1] + rgb[2] > 0.002f) ++nonblack;
    if (write_ppm) {
      // Through an unsigned integer: char(255.0f) overflows a signed char (undefined, it wrote zeros).
      for (int c = 0; c < 3; ++c) {
        float v = std::min(1.0f, std::max(0.0f, rgb[c]));
        ppm.push_back(char(static_cast<unsigned char>(uint32_t(v * 255.0f + 0.5f))));
      }
    }
  }
  double avg[3] = {sum[0] / count, sum[1] / count, sum[2] / count};
  REXGPU_INFO("[sdnx_front] addr={:#x} {}x{}: avg=({:.4f},{:.4f},{:.4f}) max=({:.3f},{:.3f},{:.3f}) nonblack={:.1f}%",
              dump_address_,
              dump_width_, dump_height_, avg[0], avg[1], avg[2], maxv[0], maxv[1], maxv[2],
              100.0 * double(nonblack) / double(count));
  if (write_ppm && !ppm.empty()) {
    std::filesystem::path path = rex::filesystem::GetExecutableFolder() / "front_dump.ppm";
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", dump_width_, dump_height_);
      std::fwrite(ppm.data(), 1, ppm.size(), f);
      std::fclose(f);
      REXGPU_INFO("[sdnx_front] wrote {}", path.string());
    }
  }
  ++dump_index_;
  --dump_remaining_;
}

#if REX_PLATFORM_SWITCH
// Guest presents for the Switch profiler (switch_perf.cpp): the "game N fps" of rex_profile.log and the FPS /
// heartbeat of the console overlays (Status Monitor, Ultrahand) through SaltyNX. The Vulkan backend counts them in
// its IssueSwap; without this the overlays showed nothing for the native renderer.
extern "C" void RexSwitchPerfCount(unsigned id);
#endif

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  ++stats_.swaps;
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(0);
#endif
  {
    static const uint64_t log_frame = std::getenv("SDNX_LOG_FRAME") ? std::strtoull(std::getenv("SDNX_LOG_FRAME"), nullptr, 10) : 0;
    if (log_frame_) REXGPU_INFO("[sdnx_frame] ---- end of swap {} ----", swap_count_);
    ++swap_count_;
    log_frame_ = log_frame && swap_count_ == log_frame;
    // SDNX_LOG_AT / SDNX_DUMP_AT = seconds after the first swap: the frame log / one PPM of that moment (swap
    // numbers do not map to time - loading screens swap far faster than 60 per second).
    static const auto first_swap = std::chrono::steady_clock::now();
    static const double log_at = std::getenv("SDNX_LOG_AT") ? std::atof(std::getenv("SDNX_LOG_AT")) : -1.0;
    static const double dump_at = std::getenv("SDNX_DUMP_AT") ? std::atof(std::getenv("SDNX_DUMP_AT")) : -1.0;
    static bool log_at_done = false, dump_at_done = false;
    double since_first = std::chrono::duration<double>(std::chrono::steady_clock::now() - first_swap).count();
    if (log_at >= 0.0 && !log_at_done && since_first >= log_at) {
      log_at_done = true;
      log_frame_ = true;
    }
    if (dump_at >= 0.0 && !dump_at_done && since_first >= dump_at) {
      dump_at_done = true;
      dump_remaining_ = 1;
    }
    log_draw_ = 0;
    if (log_frame_) REXGPU_INFO("[sdnx_frame] ---- swap {} ----", swap_count_);
  }
  ReportStats();
  EnsureFrame();
  EndRendering();
  // The front buffer: texture fetch constant 0, which Direct3D points at it before the swap.
  xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(0);
  uint32_t front_address = (uint32_t(fetch.base_address) << 12) & 0x1FFFFFFF;
  front_address_ = front_address;
  auto front_it = resolved_.find(front_address);
  Image* front = front_it != resolved_.end() ? &front_it->second : nullptr;
  if (front) ++stats_.front_found;

  // Diagnostic: SDNX_DUMP_FRONT=N copies the presented image back for N swaps (statistics + one PPM).
  static bool dump_env_read = false;
  if (!dump_env_read) {
    dump_env_read = true;
    if (const char* env = std::getenv("SDNX_DUMP_FRONT")) {
      dump_remaining_ = std::atoi(env);
      if (dump_remaining_ > 0) {
        REXGPU_INFO("[sdnx_front] dumping the front buffer for {} swaps", dump_remaining_);
      }
    }
  }
  static const bool dump_rt = std::getenv("SDNX_DUMP_RT") != nullptr;
  if (dump_rt && last_color_rt_) {
    front = last_color_rt_;
    front_address_ = 0xFFFFFFF0u;
  }
  // SDNX_DUMP_ADDR=0x...: dump the resolved image of that address instead of the front buffer.
  static const uint32_t dump_addr =
      std::getenv("SDNX_DUMP_ADDR") ? uint32_t(std::strtoul(std::getenv("SDNX_DUMP_ADDR"), nullptr, 16)) : 0;
  if (dump_addr) {
    auto it = resolved_.find(dump_addr);
    if (it != resolved_.end() && it->second.image) {
      front = &it->second;
      front_address_ = dump_addr;
    }
  }
  static const bool dump_nth = std::getenv("SDNX_DUMP_NTH") != nullptr;
  if (dump_nth) {
    // The resolve of the next frame is dumped (DumpResolveIfRequested).
    if (dump_remaining_ > 0 && !dump_pending_ && !dump_nth_frame_) {
      dump_nth_frame_ = true;
      dump_nth_count_ = 0;
    }
  } else if (front && dump_remaining_ > 0 && !dump_pending_) {
    BeginFrontDump(*front);
  }

  ui::Presenter* presenter = graphics_system_->presenter();
  bool submitted = false;
  if (presenter && front) {
    uint32_t width = std::max(frontbuffer_width, uint32_t(1));
    uint32_t height = std::max(frontbuffer_height, uint32_t(1));
    presenter->RefreshGuestOutput(
        width, height, width, height,
        [this, front, width, height, &submitted](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vulkan_context =
              static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
          const ui::vulkan::VulkanDevice* device = vulkan_device();
          const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
          uint32_t slot = uint32_t(frame_ % kFramesInFlight);
          VkDescriptorSet set = present_sets_[slot];
          VkDescriptorImageInfo source_info = {VK_NULL_HANDLE, front->view, VK_IMAGE_LAYOUT_GENERAL};
          VkDescriptorImageInfo sampler_info = {linear_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
          VkDescriptorImageInfo output_info = {VK_NULL_HANDLE, vulkan_context.image_view(),
                                               VK_IMAGE_LAYOUT_GENERAL};
          VkWriteDescriptorSet writes[3] = {};
          for (int i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = uint32_t(i);
            writes[i].descriptorCount = 1;
          }
          writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          writes[0].pImageInfo = &source_info;
          writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
          writes[1].pImageInfo = &sampler_info;
          writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          writes[2].pImageInfo = &output_info;
          dfn.vkUpdateDescriptorSets(device->device(), 3, writes, 0, nullptr);

          VkImageMemoryBarrier barrier = {};
          barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          barrier.srcAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = vulkan_context.image();
          barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          FullBarrier();
          dfn.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                   0, nullptr, 0, nullptr, 1, &barrier);
          dfn.vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, present_pipeline_);
          dfn.vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_COMPUTE, present_pipeline_layout_, 0, 1, &set, 0,
                                      nullptr);
          struct {
            float scale[2];
            uint32_t size[2];
          } pc = {{float(width) / float(front->width), float(height) / float(front->height)}, {width, height}};
          dfn.vkCmdPushConstants(cb_, present_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
          dfn.vkCmdDispatch(cb_, (width + 7) / 8, (height + 7) / 8, 1);
          barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
          barrier.dstAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
          barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
          barrier.newLayout = ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
          dfn.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                                   nullptr, 1, &barrier);
          context.SetIs8bpc(front->format == VK_FORMAT_R8G8B8A8_UNORM);
          submitted = true;
          return SubmitFrame();
        });
  }
  if (!submitted) {
    SubmitFrame();
  }
  FinishFrontDump();
}

}  // namespace rex::graphics::sdnx




