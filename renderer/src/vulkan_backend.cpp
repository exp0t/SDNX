/**
 * Sleeping Dogs port (SDNX): the Vulkan backend of the native renderer. See vulkan_backend.h.
 */

#include "vulkan_backend.h"

#include <algorithm>
#include <unordered_set>

#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::sdnx {
namespace {

VkImageAspectFlags AspectFlags(Aspect aspect) {
  VkImageAspectFlags flags = 0;
  if (uint32_t(aspect) & uint32_t(Aspect::kColor)) flags |= VK_IMAGE_ASPECT_COLOR_BIT;
  if (uint32_t(aspect) & uint32_t(Aspect::kDepth)) flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if (uint32_t(aspect) & uint32_t(Aspect::kStencil)) flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
  return flags;
}

VkImageSubresourceLayers Layers(const ImageRegion& r, Aspect aspect) {
  return {AspectFlags(aspect), r.level, r.layer, r.layer_count};
}

}  // namespace

VulkanBackend::~VulkanBackend() { Shutdown(); }

void VulkanBackend::NotYet(const char* what) const {
  static std::unordered_set<const char*> reported;
  if (reported.insert(what).second) {
    REXGPU_ERROR("[sdnx_vk] {} is not behind the backend yet (the frontend records it directly)", what);
  }
}

bool VulkanBackend::Initialize() {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  VkCommandPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device_->device(), &pool_info, nullptr, &command_pool_) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo alloc_info = {};
  alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc_info.commandPool = command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = kFramesInFlight;
  if (dfn.vkAllocateCommandBuffers(device_->device(), &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
    return false;
  }
  VkFenceCreateInfo fence_info = {};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (VkFence& fence : fences_) {
    if (dfn.vkCreateFence(device_->device(), &fence_info, nullptr, &fence) != VK_SUCCESS) return false;
  }
  // Bindless heaps (sets 0-3). Textures and samplers are added while command buffers that use them are recording or
  // pending: partially bound, update after bind, update unused while pending.
  const VkDescriptorType heap_types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                          VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
  for (int i = 0; i < 4; ++i) {
    VkDescriptorSetLayoutBinding binding = {0, heap_types[i], kHeapSize,
                                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                     VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                     VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = 1;
    flags_info.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = &flags_info;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    info.bindingCount = 1;
    info.pBindings = &binding;
    if (dfn.vkCreateDescriptorSetLayout(device_->device(), &info, nullptr, &heap_layouts_[i]) != VK_SUCCESS) {
      return false;
    }
  }
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeapSize * 3},
                                   {VK_DESCRIPTOR_TYPE_SAMPLER, kHeapSize}};
  VkDescriptorPoolCreateInfo pool = {};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pool.maxSets = 4;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(device_->device(), &pool, nullptr, &heap_pool_) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = heap_pool_;
  alloc.descriptorSetCount = 4;
  alloc.pSetLayouts = heap_layouts_;
  if (dfn.vkAllocateDescriptorSets(device_->device(), &alloc, heap_sets_) != VK_SUCCESS) return false;
  samplers_.assign(kHeapSize, VK_NULL_HANDLE);
  {
    // Set 4: three dynamic UBOs (VS, PS, shared) and, for the vertex shaders that pull their vertices, the start of
    // the same buffer as storage buffer 3.
    VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
         nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 4;
    info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device_->device(), &info, nullptr, &constants_layout_) != VK_SUCCESS) {
      return false;
    }
  }
  {
    VkDescriptorSetLayout layouts[5] = {heap_layouts_[0], heap_layouts_[1], heap_layouts_[2], heap_layouts_[3],
                                        constants_layout_};
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
    VkPipelineLayoutCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 5;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (dfn.vkCreatePipelineLayout(device_->device(), &info, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;
  }
  {
    constexpr uint32_t kSets = 8;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * kSets},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSets}};
    VkDescriptorPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = kSets;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(device_->device(), &info, nullptr, &constants_pool_) != VK_SUCCESS) return false;
  }
  return true;
}

uint64_t VulkanBackend::MaxStorageBufferRange() const { return device_->properties().maxStorageBufferRange; }

VkDescriptorSet VulkanBackend::ConstantsSet(GpuBuffer* buffer, const uint32_t sizes[3]) {
  auto it = constants_sets_.find(buffer);
  if (it != constants_sets_.end()) return it->second;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  VkDescriptorSetAllocateInfo alloc = {};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = constants_pool_;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &constants_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(device_->device(), &alloc, &set) != VK_SUCCESS) {
    REXGPU_ERROR("[sdnx_vk] could not allocate a constants descriptor set");
    return VK_NULL_HANDLE;
  }
  const VkDeviceSize storage_range = std::min<VkDeviceSize>(buffer->size, MaxStorageBufferRange());
  VkDescriptorBufferInfo buffers[4] = {{buffer->buffer, 0, sizes[0]},
                                       {buffer->buffer, 0, sizes[1]},
                                       {buffer->buffer, 0, sizes[2]},
                                       {buffer->buffer, 0, storage_range}};
  VkWriteDescriptorSet writes[4] = {};
  for (uint32_t b = 0; b < 4; ++b) {
    writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[b].dstSet = set;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = b == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[b].pBufferInfo = &buffers[b];
  }
  dfn.vkUpdateDescriptorSets(device_->device(), 4, writes, 0, nullptr);
  constants_sets_.emplace(buffer, set);
  return set;
}

void VulkanBackend::Shutdown() {
  if (!command_pool_) return;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  dfn.vkDeviceWaitIdle(device_->device());
  for (auto& it : pipelines_) dfn.vkDestroyPipeline(device_->device(), it.second, nullptr);
  pipelines_.clear();
  constants_sets_.clear();
  if (constants_pool_) dfn.vkDestroyDescriptorPool(device_->device(), constants_pool_, nullptr);
  constants_pool_ = VK_NULL_HANDLE;
  if (pipeline_layout_) dfn.vkDestroyPipelineLayout(device_->device(), pipeline_layout_, nullptr);
  pipeline_layout_ = VK_NULL_HANDLE;
  if (constants_layout_) dfn.vkDestroyDescriptorSetLayout(device_->device(), constants_layout_, nullptr);
  constants_layout_ = VK_NULL_HANDLE;
  for (VkSampler& sampler : samplers_) {
    if (sampler) dfn.vkDestroySampler(device_->device(), sampler, nullptr);
  }
  samplers_.clear();
  if (heap_pool_) dfn.vkDestroyDescriptorPool(device_->device(), heap_pool_, nullptr);
  heap_pool_ = VK_NULL_HANDLE;
  for (VkDescriptorSetLayout& layout : heap_layouts_) {
    if (layout) dfn.vkDestroyDescriptorSetLayout(device_->device(), layout, nullptr);
    layout = VK_NULL_HANDLE;
  }
  for (VkFence& fence : fences_) {
    if (fence) dfn.vkDestroyFence(device_->device(), fence, nullptr);
    fence = VK_NULL_HANDLE;
  }
  dfn.vkDestroyCommandPool(device_->device(), command_pool_, nullptr);
  command_pool_ = VK_NULL_HANDLE;
  command_buffers_ = {};
  cb_ = VK_NULL_HANDLE;
}

// ---- Frames

void VulkanBackend::BeginFrame(uint32_t slot) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  dfn.vkWaitForFences(device_->device(), 1, &fences_[slot], VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(device_->device(), 1, &fences_[slot]);
  cb_ = command_buffers_[slot];
  VkCommandBufferBeginInfo begin = {};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(cb_, &begin);  // resets it (pool created with RESET_COMMAND_BUFFER_BIT)
  bound_ = {};  // nothing is bound in a new command buffer
  rendering_ = false;
}

bool VulkanBackend::SubmitFrame(uint32_t slot) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  dfn.vkEndCommandBuffer(cb_);
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cb_;
  VkResult result;
  {
    auto queue = device_->AcquireQueue(device_->queue_family_graphics_compute(), 0);
    result = dfn.vkQueueSubmit(queue.queue(), 1, &submit, fences_[slot]);
  }
  cb_ = VK_NULL_HANDLE;
  return result == VK_SUCCESS;
}

void VulkanBackend::WaitIdle() { device_->functions().vkDeviceWaitIdle(device_->device()); }

// ---- Memory

GpuBuffer* VulkanBackend::CreateBuffer(uint64_t size, BufferUsage usage, uint8_t** mapping) {
  VkBufferUsageFlags flags = 0;
  ui::vulkan::util::MemoryPurpose purpose = ui::vulkan::util::MemoryPurpose::kUpload;
  switch (usage) {
    case BufferUsage::kUpload:
      flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      break;
    case BufferUsage::kGeometry:
      flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
      break;
    case BufferUsage::kReadback:
      flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      purpose = ui::vulkan::util::MemoryPurpose::kReadback;
      break;
    case BufferUsage::kDeviceLocal:
      flags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      purpose = ui::vulkan::util::MemoryPurpose::kDeviceLocal;
      break;
  }
  auto* buffer = new GpuBuffer();
  uint32_t memory_type;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(device_, size, flags, purpose, buffer->buffer,
                                                         buffer->memory, &memory_type)) {
    delete buffer;
    return nullptr;
  }
  void* mapped = nullptr;
  if (usage != BufferUsage::kDeviceLocal) {
    device_->functions().vkMapMemory(device_->device(), buffer->memory, 0, VK_WHOLE_SIZE, 0, &mapped);
  }
  buffer->mapping = static_cast<uint8_t*>(mapped);
  buffer->size = size;
  if (mapping) *mapping = buffer->mapping;
  return buffer;
}

void VulkanBackend::DestroyBuffer(GpuBuffer* buffer) {
  if (!buffer) return;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (buffer->buffer) dfn.vkDestroyBuffer(device_->device(), buffer->buffer, nullptr);
  if (buffer->memory) dfn.vkFreeMemory(device_->device(), buffer->memory, nullptr);
  delete buffer;
}

uint64_t VulkanBackend::ConstantAlignment() const {
  return std::max<uint64_t>(device_->properties().minUniformBufferOffsetAlignment, 16);
}

// ---- Images

GpuImage* VulkanBackend::CreateImage(const ImageDesc& desc) {
  VkImageCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = desc.type == ImageType::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.flags = (desc.type == ImageType::kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
               (desc.mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
  info.format = desc.format;
  info.extent = {desc.width, desc.height, desc.type == ImageType::k3D ? desc.depth : 1};
  info.mipLevels = desc.levels;
  info.arrayLayers = desc.type == ImageType::kCube ? 6 : desc.layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               (desc.depth_stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                   : (desc.render_target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0));
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  auto* image = new GpuImage();
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(device_, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                        image->image, image->memory)) {
    REXGPU_ERROR("[sdnx_vk] could not create a {}x{} image (format {})", desc.width, desc.height, int(desc.format));
    delete image;
    return nullptr;
  }
  image->desc = desc;
  image->aspect = desc.depth_stencil ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                     : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageViewCreateInfo view = {};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = image->image;
  view.viewType = desc.type == ImageType::k3D     ? VK_IMAGE_VIEW_TYPE_3D
                  : desc.type == ImageType::kCube ? VK_IMAGE_VIEW_TYPE_CUBE
                                                  : VK_IMAGE_VIEW_TYPE_2D;
  view.format = desc.format;
  view.subresourceRange = {image->aspect, 0, desc.levels, 0, desc.type == ImageType::kCube ? 6u : desc.layers};
  device_->functions().vkCreateImageView(device_->device(), &view, nullptr, &image->view);
  return image;
}

void VulkanBackend::DestroyImage(GpuImage* image) {
  if (!image) return;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  if (image->view) dfn.vkDestroyImageView(device_->device(), image->view, nullptr);
  if (image->image) dfn.vkDestroyImage(device_->device(), image->image, nullptr);
  if (image->memory) dfn.vkFreeMemory(device_->device(), image->memory, nullptr);
  delete image;
}

void VulkanBackend::InitializeImage(GpuImage* image, const ImageDesc& desc, bool clear) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  const uint32_t layers = desc.type == ImageType::kCube ? 6 : desc.layers;
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image->image;
  barrier.subresourceRange = {image->aspect, 0, desc.levels, 0, layers};
  dfn.vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &barrier);
  if (!clear) return;
  // New memory has undefined contents: start colour at 0 and depth at 1 (far), as after a clear.
  if (image->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
    VkClearDepthStencilValue ds = {1.0f, 0};
    dfn.vkCmdClearDepthStencilImage(cb_, image->image, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &barrier.subresourceRange);
  } else {
    VkClearColorValue color = {};
    dfn.vkCmdClearColorImage(cb_, image->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &barrier.subresourceRange);
  }
}

GpuView* VulkanBackend::CreateView(GpuImage* image, const ViewDesc& desc) {
  VkImageViewCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image->image;
  info.viewType = desc.type == ImageType::kCube  ? VK_IMAGE_VIEW_TYPE_CUBE
                  : desc.type == ImageType::k3D ? VK_IMAGE_VIEW_TYPE_3D
                                                : VK_IMAGE_VIEW_TYPE_2D;
  info.format = desc.format;
  info.components = {desc.swizzle[0], desc.swizzle[1], desc.swizzle[2], desc.swizzle[3]};
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.levels, 0,
                           desc.type == ImageType::kCube ? 6u : std::min(desc.layers, 1u)};
  auto* view = new GpuView();
  if (device_->functions().vkCreateImageView(device_->device(), &info, nullptr, &view->view) != VK_SUCCESS) {
    delete view;
    return nullptr;
  }
  return view;
}

void VulkanBackend::DestroyView(GpuView* view) {
  if (!view) return;
  if (view->view) device_->functions().vkDestroyImageView(device_->device(), view->view, nullptr);
  delete view;
}

bool VulkanBackend::FormatFilterable(VkFormat format) {
  auto it = filterable_.find(format);
  if (it != filterable_.end()) return it->second;
  VkFormatProperties properties = {};
  device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(device_->physical_device(), format,
                                                                              &properties);
  bool filterable = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  if (!filterable) {
    REXGPU_WARN("[sdnx_native] format {} cannot be sampled with linear filtering here: point sampling it", int(format));
  }
  filterable_[format] = filterable;
  return filterable;
}

void VulkanBackend::WriteImageDescriptor(ImageType heap, uint32_t index, GpuView* view) {
  VkDescriptorImageInfo image_info = {VK_NULL_HANDLE, view ? view->view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = heap_sets_[uint32_t(heap)];
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image_info;
  device_->functions().vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void VulkanBackend::WriteSamplerDescriptor(uint32_t index, const SamplerDesc& desc) {
  if (index >= samplers_.size()) return;
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  VkSamplerCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = desc.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = desc.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = desc.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = desc.address[0];
  info.addressModeV = desc.address[1];
  info.addressModeW = desc.address[2];
  info.mipLodBias = desc.lod_bias;
  info.minLod = 0.0f;
  info.maxLod = desc.base_map_only ? 0.25f : VK_LOD_CLAMP_NONE;
  info.borderColor = desc.border;
  VkSampler sampler;
  if (dfn.vkCreateSampler(device_->device(), &info, nullptr, &sampler) != VK_SUCCESS) return;
  // A sampler index is written once (the frontend never reuses one); an overwrite would need the old sampler kept
  // until the frames that use it are done.
  if (samplers_[index]) dfn.vkDestroySampler(device_->device(), samplers_[index], nullptr);
  samplers_[index] = sampler;
  VkDescriptorImageInfo image_info = {sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  VkWriteDescriptorSet write = {};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = heap_sets_[3];
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  write.pImageInfo = &image_info;
  dfn.vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void VulkanBackend::ClearColor(GpuImage* image, const ImageRegion& region, const float rgba[4]) {
  VkClearColorValue color;
  for (int i = 0; i < 4; ++i) color.float32[i] = rgba[i];
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, region.level, 1, region.layer, region.layer_count};
  device_->functions().vkCmdClearColorImage(cb_, image->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

void VulkanBackend::ClearDepthStencil(GpuImage* image, const ImageRegion& region, Aspect aspects, float depth,
                                      uint32_t stencil) {
  VkClearDepthStencilValue value = {depth, stencil};
  VkImageSubresourceRange range = {AspectFlags(aspects), region.level, 1, region.layer, region.layer_count};
  device_->functions().vkCmdClearDepthStencilImage(cb_, image->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

void VulkanBackend::CopyImage(GpuImage* source, const ImageRegion& from, GpuImage* destination,
                              const ImageRegion& to, Aspect aspect) {
  VkImageCopy region = {};
  region.srcSubresource = Layers(from, aspect);
  region.srcOffset = {from.x, from.y, from.z};
  region.dstSubresource = Layers(to, aspect);
  region.dstOffset = {to.x, to.y, to.z};
  region.extent = {from.width, from.height, from.depth};
  device_->functions().vkCmdCopyImage(cb_, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image,
                                      VK_IMAGE_LAYOUT_GENERAL, 1, &region);
}

void VulkanBackend::BlitImage(GpuImage* source, const ImageRegion& from, GpuImage* destination,
                              const ImageRegion& to, Aspect aspect, bool linear) {
  VkImageBlit blit = {};
  blit.srcSubresource = Layers(from, aspect);
  blit.srcOffsets[0] = {from.x, from.y, from.z};
  blit.srcOffsets[1] = {from.x + int32_t(from.width), from.y + int32_t(from.height), from.z + int32_t(from.depth)};
  blit.dstSubresource = Layers(to, aspect);
  blit.dstOffsets[0] = {to.x, to.y, to.z};
  blit.dstOffsets[1] = {to.x + int32_t(to.width), to.y + int32_t(to.height), to.z + int32_t(to.depth)};
  device_->functions().vkCmdBlitImage(cb_, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image,
                                      VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                                      linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

void VulkanBackend::CopyBufferToImage(BufferRef source, uint32_t row_length, uint32_t image_height,
                                      GpuImage* destination, const ImageRegion& to, Aspect aspect) {
  VkBufferImageCopy region = {};
  region.bufferOffset = source.offset;
  region.bufferRowLength = row_length;
  region.bufferImageHeight = image_height;
  region.imageSubresource = Layers(to, aspect);
  region.imageOffset = {to.x, to.y, to.z};
  region.imageExtent = {to.width, to.height, to.depth};
  device_->functions().vkCmdCopyBufferToImage(cb_, source.buffer->buffer, destination->image, VK_IMAGE_LAYOUT_GENERAL,
                                              1, &region);
}

void VulkanBackend::CopyImageToBuffer(GpuImage* source, const ImageRegion& from, Aspect aspect,
                                      BufferRef destination, uint32_t row_length) {
  VkBufferImageCopy region = {};
  region.bufferOffset = destination.offset;
  region.bufferRowLength = row_length;
  region.imageSubresource = Layers(from, aspect);
  region.imageOffset = {from.x, from.y, from.z};
  region.imageExtent = {from.width, from.height, from.depth};
  device_->functions().vkCmdCopyImageToBuffer(cb_, source->image, VK_IMAGE_LAYOUT_GENERAL,
                                              destination.buffer->buffer, 1, &region);
}

void VulkanBackend::CopyBuffer(BufferRef source, BufferRef destination, uint64_t size) {
  VkBufferCopy copy = {source.offset, destination.offset, size};
  device_->functions().vkCmdCopyBuffer(cb_, source.buffer->buffer, destination.buffer->buffer, 1, &copy);
}

// ---- Ordering

void VulkanBackend::FullBarrier() {
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  device_->functions().vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0,
                                            nullptr);
}

void VulkanBackend::AttachmentBarrier() {
  constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  VkMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  device_->functions().vkCmdPipelineBarrier(cb_, kStages, kStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

// ---- Not behind the interface yet (portlog 16.47)

bool VulkanBackend::CreateShader(GpuShader*& shader, bool vertex, const void* code, uint64_t size,
                                 const char* name) {
  (void)vertex;
  (void)name;
  VkShaderModule module = ui::vulkan::util::CreateShaderModule(device_, static_cast<const uint32_t*>(code), size_t(size));
  if (!module) return false;
  shader = new GpuShader();
  shader->module = module;
  return true;
}

void VulkanBackend::DestroyShader(GpuShader* shader) {
  if (!shader) return;
  if (shader->module) device_->functions().vkDestroyShaderModule(device_->device(), shader->module, nullptr);
  delete shader;
}

void VulkanBackend::BeginRendering(GpuImage* const colors[4], uint32_t color_count, GpuImage* depth, uint32_t width,
                                   uint32_t height) {
  VkRenderingAttachmentInfo color_infos[4] = {};
  for (uint32_t i = 0; i < color_count; ++i) {
    color_infos[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_infos[i].imageView = colors[i] ? colors[i]->view : VK_NULL_HANDLE;
    color_infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color_infos[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_infos[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  VkRenderingAttachmentInfo depth_info = {};
  depth_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depth_info.imageView = depth ? depth->view : VK_NULL_HANDLE;
  depth_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  depth_info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering_info = {};
  rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering_info.renderArea = {{0, 0}, {width, height}};
  rendering_info.layerCount = 1;
  rendering_info.colorAttachmentCount = color_count;
  rendering_info.pColorAttachments = color_infos;
  rendering_info.pDepthAttachment = depth ? &depth_info : nullptr;
  rendering_info.pStencilAttachment = depth ? &depth_info : nullptr;
  device_->functions().vkCmdBeginRendering(cb_, &rendering_info);
  rendering_ = true;
}

void VulkanBackend::EndRendering() {
  if (!rendering_) return;
  device_->functions().vkCmdEndRendering(cb_);
  rendering_ = false;
}

void VulkanBackend::ClearAttachmentRect(GpuImage* target, const VkRect2D& rect, const float rgba[4]) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  VkRenderingAttachmentInfo color_info = {};
  color_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color_info.imageView = target->view;
  color_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  color_info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering_info = {};
  rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering_info.renderArea = {{0, 0}, {target->desc.width, target->desc.height}};
  rendering_info.layerCount = 1;
  rendering_info.colorAttachmentCount = 1;
  rendering_info.pColorAttachments = &color_info;
  dfn.vkCmdBeginRendering(cb_, &rendering_info);
  VkClearAttachment clear = {};
  clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  for (int i = 0; i < 4; ++i) clear.clearValue.color.float32[i] = rgba[i];
  VkClearRect clear_rect = {rect, 0, 1};
  dfn.vkCmdClearAttachments(cb_, 1, &clear, 1, &clear_rect);
  dfn.vkCmdEndRendering(cb_);
}

namespace {

VkBlendFactor BlendFactor(uint32_t f) {
  // Xenos BlendFactor -> Vulkan.
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

}  // namespace

bool VulkanBackend::Draw(const DrawPacket& p) {
  const ui::vulkan::VulkanDevice::Functions& dfn = device_->functions();
  struct PipelineKey {
    uint64_t vs, ps;
    uint32_t topology, restart, spec, attribute_count, binding_count;
    uint32_t attr[16][4];
    uint32_t bind[16];
    uint32_t color_formats[4], depth_format, color_count;
    uint32_t blend[4], color_mask;
    uint32_t depth_test, depth_write, depth_func;
    uint32_t stencil;
    uint32_t cull;
  } key = {};
  key.vs = reinterpret_cast<uintptr_t>(p.vs);
  key.ps = reinterpret_cast<uintptr_t>(p.ps);
  key.topology = p.topology;
  key.restart = p.primitive_restart;
  key.spec = p.spec;
  key.attribute_count = p.attribute_count;
  key.binding_count = p.binding_count;
  for (uint32_t a = 0; a < p.attribute_count; ++a) {
    key.attr[a][0] = p.attributes[a].location;
    key.attr[a][1] = p.attributes[a].binding;
    key.attr[a][2] = p.attributes[a].format;
    key.attr[a][3] = p.attributes[a].offset;
  }
  for (uint32_t b = 0; b < p.binding_count; ++b) key.bind[b] = p.strides[b];
  key.color_count = p.color_count;
  for (uint32_t i = 0; i < p.color_count; ++i) {
    key.color_formats[i] = p.colors[i] ? p.colors[i]->desc.format : VK_FORMAT_UNDEFINED;
    key.blend[i] = p.blend_control[i];
  }
  key.color_mask = p.color_mask;
  key.depth_format = p.depth ? p.depth->desc.format : VK_FORMAT_UNDEFINED;
  key.depth_test = p.depth_test;
  key.depth_write = p.depth_write;
  key.depth_func = p.depth_func;
  key.stencil = p.stencil;
  key.cull = p.cull;
  const uint64_t pipeline_hash = XXH3_64bits(&key, sizeof(key));
  VkPipeline& pipeline = pipelines_[pipeline_hash];
  if (!pipeline) {
    ++pipelines_created_;
    VkSpecializationMapEntry spec_entry = {0, 0, 4};
    VkSpecializationInfo spec_info = {1, &spec_entry, 4, &p.spec};
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p.vs->module;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = &spec_info;
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p.ps ? p.ps->module : VK_NULL_HANDLE;
    VkVertexInputAttributeDescription vk_attributes[16];
    VkVertexInputBindingDescription vk_bindings[16];
    for (uint32_t a = 0; a < p.attribute_count; ++a) {
      vk_attributes[a] = {p.attributes[a].location, p.attributes[a].binding, p.attributes[a].format,
                          p.attributes[a].offset};
    }
    for (uint32_t b = 0; b < p.binding_count; ++b) vk_bindings[b] = {b, p.strides[b], VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = p.binding_count;
    vertex_input.pVertexBindingDescriptions = vk_bindings;
    vertex_input.vertexAttributeDescriptionCount = p.attribute_count;
    vertex_input.pVertexAttributeDescriptions = vk_attributes;
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = p.topology;
    input_assembly.primitiveRestartEnable = p.primitive_restart;
    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = ((p.cull & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((p.cull & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    raster.frontFace = (p.cull & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = p.depth ? VK_TRUE : VK_FALSE;  // the factors are dynamic (zero for most draws)
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = p.depth_test;
    depth_stencil.depthWriteEnable = p.depth_write;
    depth_stencil.depthCompareOp = p.depth_func;
    if (p.stencil) {
      // RB_DEPTHCONTROL: Xenos CompareFunction and StencilOp have the Vulkan values. Reference and masks are dynamic.
      const uint32_t s = p.stencil;
      depth_stencil.stencilTestEnable = VK_TRUE;
      depth_stencil.front.compareOp = VkCompareOp((s >> 8) & 7);
      depth_stencil.front.failOp = VkStencilOp((s >> 11) & 7);
      depth_stencil.front.passOp = VkStencilOp((s >> 14) & 7);
      depth_stencil.front.depthFailOp = VkStencilOp((s >> 17) & 7);
      if ((s >> 7) & 1) {
        depth_stencil.back.compareOp = VkCompareOp((s >> 20) & 7);
        depth_stencil.back.failOp = VkStencilOp((s >> 23) & 7);
        depth_stencil.back.passOp = VkStencilOp((s >> 26) & 7);
        depth_stencil.back.depthFailOp = VkStencilOp((s >> 29) & 7);
      } else {
        depth_stencil.back = depth_stencil.front;
      }
    }
    VkPipelineColorBlendAttachmentState blend[4] = {};
    for (uint32_t i = 0; i < p.color_count; ++i) {
      const uint32_t bc = p.blend_control[i];  // RB_BLENDCONTROL
      blend[i].colorWriteMask = (p.color_mask >> (4 * i)) & 0xF;
      blend[i].srcColorBlendFactor = BlendFactor(bc & 0x1F);
      blend[i].colorBlendOp = BlendOp((bc >> 5) & 7);
      blend[i].dstColorBlendFactor = BlendFactor((bc >> 8) & 0x1F);
      blend[i].srcAlphaBlendFactor = BlendFactor((bc >> 16) & 0x1F);
      blend[i].alphaBlendOp = BlendOp((bc >> 21) & 7);
      blend[i].dstAlphaBlendFactor = BlendFactor((bc >> 24) & 0x1F);
      blend[i].blendEnable = !(blend[i].srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
                               blend[i].dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                               blend[i].colorBlendOp == VK_BLEND_OP_ADD &&
                               blend[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                               blend[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
                               blend[i].alphaBlendOp == VK_BLEND_OP_ADD);
      if (!p.colors[i]) blend[i].colorWriteMask = 0;
    }
    VkPipelineColorBlendStateCreateInfo color_blend = {};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = p.color_count;
    color_blend.pAttachments = blend;
    VkDynamicState dynamic_states[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                        VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 7;
    dynamic.pDynamicStates = dynamic_states;
    VkFormat color_formats[4];
    for (uint32_t i = 0; i < p.color_count; ++i) color_formats[i] = VkFormat(key.color_formats[i]);
    VkPipelineRenderingCreateInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = p.color_count;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = VkFormat(key.depth_format);
    rendering.stencilAttachmentFormat = VkFormat(key.depth_format);
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = p.ps ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    if (dfn.vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
      pipelines_.erase(pipeline_hash);
      REXGPU_ERROR("[sdnx_vk] pipeline creation failed");
      return false;
    }
  }

  // Only what changed since the previous draw of this command buffer.
  if (bound_.pipeline != pipeline) {
    dfn.vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_.pipeline = pipeline;
  }
  if (!bound_.viewport_set || std::memcmp(&bound_.viewport, &p.viewport, sizeof(p.viewport))) {
    dfn.vkCmdSetViewport(cb_, 0, 1, &p.viewport);
    bound_.viewport = p.viewport;
    bound_.viewport_set = true;
  }
  if (!bound_.scissor_set || std::memcmp(&bound_.scissor, &p.scissor, sizeof(p.scissor))) {
    dfn.vkCmdSetScissor(cb_, 0, 1, &p.scissor);
    bound_.scissor = p.scissor;
    bound_.scissor_set = true;
  }
  if (std::memcmp(bound_.blend, p.blend_constants, sizeof(p.blend_constants))) {
    dfn.vkCmdSetBlendConstants(cb_, p.blend_constants);
    std::memcpy(bound_.blend, p.blend_constants, sizeof(p.blend_constants));
  }
  {
    const uint32_t* s = p.stencil_values;
    if (s[0] != bound_.stencil[0]) dfn.vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_BIT, s[0]);
    if (s[1] != bound_.stencil[1]) dfn.vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_BACK_BIT, s[1]);
    if (s[2] != bound_.stencil[2]) dfn.vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_FRONT_BIT, s[2]);
    if (s[3] != bound_.stencil[3]) dfn.vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_BACK_BIT, s[3]);
    if (s[4] != bound_.stencil[4]) dfn.vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_FRONT_BIT, s[4]);
    if (s[5] != bound_.stencil[5]) dfn.vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_BACK_BIT, s[5]);
    std::memcpy(bound_.stencil, s, sizeof(bound_.stencil));
  }
  if (p.depth && (p.depth_bias_constant != bound_.bias[0] || p.depth_bias_slope != bound_.bias[1])) {
    dfn.vkCmdSetDepthBias(cb_, p.depth_bias_constant, 0.0f, p.depth_bias_slope);
    bound_.bias[0] = p.depth_bias_constant;
    bound_.bias[1] = p.depth_bias_slope;
  }
  if (!bound_.heaps) {
    // The bindless heaps never change within a command buffer.
    dfn.vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 4, heap_sets_, 0, nullptr);
    bound_.heaps = true;
  }
  VkDescriptorSet constants_set = ConstantsSet(p.constants[0].buffer, p.constant_sizes);
  if (!constants_set) return false;
  const uint32_t dynamic_offsets[3] = {uint32_t(p.constants[0].offset), uint32_t(p.constants[1].offset),
                                       uint32_t(p.constants[2].offset)};
  // Same set and offsets as the previous draw of this command buffer (reused constants): it stays bound.
  if (bound_.constants_set != constants_set || std::memcmp(bound_.constants_offsets, dynamic_offsets, 12) != 0) {
    dfn.vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 4, 1, &constants_set, 3,
                                dynamic_offsets);
    bound_.constants_set = constants_set;
    std::memcpy(bound_.constants_offsets, dynamic_offsets, 12);
  } else {
    ++constants_bind_skipped_;
  }
  if (p.binding_count) {
    VkBuffer buffers[16];
    VkDeviceSize offsets[16];
    for (uint32_t b = 0; b < p.binding_count; ++b) {
      buffers[b] = p.vertex_buffers[b].buffer->buffer;
      offsets[b] = p.vertex_buffers[b].offset;
    }
    // Rebind from the first binding that differs.
    uint32_t first = 0;
    while (first < p.binding_count && first < bound_.vertex_count && bound_.vertex_buffers[first] == buffers[first] &&
           bound_.vertex_offsets[first] == offsets[first]) {
      ++first;
    }
    if (first < p.binding_count) {
      dfn.vkCmdBindVertexBuffers(cb_, first, p.binding_count - first, buffers + first, offsets + first);
    }
    for (uint32_t b = 0; b < p.binding_count; ++b) {
      bound_.vertex_buffers[b] = buffers[b];
      bound_.vertex_offsets[b] = offsets[b];
    }
    bound_.vertex_count = std::max(bound_.vertex_count, p.binding_count);
  }
  if (p.index_count) {
    VkBuffer index_buffer = p.index_buffer.buffer->buffer;
    if (bound_.index_buffer != index_buffer || bound_.index_offset != p.index_buffer.offset) {
      dfn.vkCmdBindIndexBuffer(cb_, index_buffer, p.index_buffer.offset, VK_INDEX_TYPE_UINT32);
      bound_.index_buffer = index_buffer;
      bound_.index_offset = p.index_buffer.offset;
    }
    dfn.vkCmdDrawIndexed(cb_, p.index_count, 1, 0, p.vertex_offset, 0);
  } else {
    dfn.vkCmdDraw(cb_, p.vertex_count, 1, 0, 0);
  }
  return true;
}

bool VulkanBackend::BeginQuery(uint32_t, uint32_t) {
  NotYet("BeginQuery");
  return false;
}
void VulkanBackend::EndQuery(uint32_t, uint32_t) { NotYet("EndQuery"); }
void VulkanBackend::ResetQueries(uint32_t, uint32_t) { NotYet("ResetQueries"); }
void VulkanBackend::ResolveQueries(uint32_t, uint32_t) { NotYet("ResolveQueries"); }
const uint64_t* VulkanBackend::QueryResults(uint32_t) {
  NotYet("QueryResults");
  return nullptr;
}
bool VulkanBackend::Present(uint32_t, GpuImage*, uint32_t, uint32_t, uint32_t, uint32_t, bool) {
  NotYet("Present");
  return false;
}

}  // namespace rex::graphics::sdnx
