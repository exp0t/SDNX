/**
 * Sleeping Dogs port (SDNX): the Vulkan backend of the native renderer (PC bench, NVK on the Switch).
 * See rex/graphics/sdnx/gpu_backend.h.
 *
 * Migration in steps (portlog 16.47): frames, barriers, images and buffers are here; the frontend still records the
 * other commands into command_buffer() directly until they move behind the interface too.
 */
#pragma once

#include <array>
#include <unordered_map>
#include <vector>

#include <rex/graphics/sdnx/gpu_backend.h>
#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::sdnx {

struct GpuImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // default view: the whole image, its format (2D: as an attachment too)
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  ImageDesc desc;
};

struct GpuBuffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* mapping = nullptr;
  uint64_t size = 0;
};

struct GpuView {
  VkImageView view = VK_NULL_HANDLE;
};

struct GpuShader {
  VkShaderModule module = VK_NULL_HANDLE;
};

class VulkanBackend final : public GpuBackend {
 public:
  explicit VulkanBackend(const ui::vulkan::VulkanDevice* device) : device_(device) {}
  ~VulkanBackend() override;

  bool Initialize() override;
  void Shutdown() override;
  const char* name() const override { return "Vulkan"; }
  const char* ShaderPackage() const override { return "sdnx_shaders"; }
  const char* ShaderExtension() const override { return ".spv"; }
  uint64_t MaxStorageBufferRange() const override;
  uint64_t pipelines_created() const override { return pipelines_created_; }

  void BeginFrame(uint32_t slot) override;
  bool SubmitFrame(uint32_t slot) override;
  void WaitIdle() override;

  GpuBuffer* CreateBuffer(uint64_t size, BufferUsage usage, uint8_t** mapping) override;
  void DestroyBuffer(GpuBuffer* buffer) override;
  uint64_t ConstantAlignment() const override;

  GpuImage* CreateImage(const ImageDesc& desc) override;
  void DestroyImage(GpuImage* image) override;
  void InitializeImage(GpuImage* image, const ImageDesc& desc, bool clear) override;
  GpuView* CreateView(GpuImage* image, const ViewDesc& desc) override;
  void DestroyView(GpuView* view) override;
  bool FormatFilterable(VkFormat format) override;

  void WriteImageDescriptor(ImageType heap, uint32_t index, GpuView* view) override;
  void WriteSamplerDescriptor(uint32_t index, const SamplerDesc& desc) override;

  void ClearColor(GpuImage* image, const ImageRegion& region, const float rgba[4]) override;
  void ClearDepthStencil(GpuImage* image, const ImageRegion& region, Aspect aspects, float depth,
                         uint32_t stencil) override;
  void CopyImage(GpuImage* source, const ImageRegion& from, GpuImage* destination, const ImageRegion& to,
                 Aspect aspect) override;
  void BlitImage(GpuImage* source, const ImageRegion& from, GpuImage* destination, const ImageRegion& to,
                 Aspect aspect, bool linear) override;
  void CopyBufferToImage(BufferRef source, uint32_t row_length, uint32_t image_height, GpuImage* destination,
                         const ImageRegion& to, Aspect aspect) override;
  void CopyImageToBuffer(GpuImage* source, const ImageRegion& from, Aspect aspect, BufferRef destination,
                         uint32_t row_length) override;
  void CopyBuffer(BufferRef source, BufferRef destination, uint64_t size) override;

  void FullBarrier() override;
  void AttachmentBarrier() override;

  bool CreateShader(GpuShader*& shader, bool vertex, const void* code, uint64_t size, const char* name) override;
  void DestroyShader(GpuShader* shader) override;
  void BeginRendering(GpuImage* const colors[4], uint32_t color_count, GpuImage* depth, uint32_t width,
                      uint32_t height) override;
  bool Draw(const DrawPacket& packet) override;
  void EndRendering() override;
  void ClearAttachmentRect(GpuImage* target, const VkRect2D& rect, const float rgba[4]) override;

  bool BeginQuery(uint32_t slot, uint32_t index) override;
  void EndQuery(uint32_t slot, uint32_t index) override;
  void ResetQueries(uint32_t slot, uint32_t count) override;
  void ResolveQueries(uint32_t slot, uint32_t count) override;
  const uint64_t* QueryResults(uint32_t slot) override;

  bool Present(uint32_t slot, GpuImage* front, uint32_t front_width, uint32_t front_height, uint32_t width,
               uint32_t height, bool is_8bpc) override;

  // Transitional (until every command is behind the interface): the command buffer being recorded, the device, the
  // bindless heap sets 0-3 and their layouts (the frontend still makes the draw pipeline layout and binds them).
  VkCommandBuffer command_buffer() const { return cb_; }
  const ui::vulkan::VulkanDevice* device() const { return device_; }
  // Statistics (SDNX_PROF): draws whose constants set stayed bound.
  uint64_t constants_bind_skipped() const { return constants_bind_skipped_; }

 private:
  void NotYet(const char* what) const;

  const ui::vulkan::VulkanDevice* device_;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kFramesInFlight> command_buffers_{};
  std::array<VkFence, kFramesInFlight> fences_{};
  VkCommandBuffer cb_ = VK_NULL_HANDLE;
  // Set 4 of a buffer: its blocks as 3 dynamic UBOs (sizes from the first draw that uses it) and its start as
  // storage buffer 3. One per upload ring, made on first use.
  VkDescriptorSet ConstantsSet(GpuBuffer* buffer, const uint32_t sizes[3]);

  // Bindless heaps: sets 0-2 sampled images (2D, 3D, cube), set 3 samplers; kHeapSize each, update after bind.
  VkDescriptorSetLayout heap_layouts_[4] = {};
  VkDescriptorPool heap_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet heap_sets_[4] = {};
  std::vector<VkSampler> samplers_;  // by sampler index (VK_NULL_HANDLE: not written)
  std::unordered_map<int, bool> filterable_;

  VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool constants_pool_ = VK_NULL_HANDLE;
  std::unordered_map<GpuBuffer*, VkDescriptorSet> constants_sets_;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  uint64_t pipelines_created_ = 0;
  uint64_t constants_bind_skipped_ = 0;
  bool rendering_ = false;
  // Graphics state already recorded in the current command buffer (it survives rendering passes, copies and clears;
  // the output uses the compute bind point): only changes are recorded. Each vkCmdSet* costs CPU time in the driver
  // (NVK copies and flushes its dynamic state). Reset by BeginFrame.
  struct BoundState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkViewport viewport = {};
    VkRect2D scissor = {};
    float blend[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    uint32_t stencil[6] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    float bias[2] = {-1e30f, -1e30f};
    bool heaps = false, viewport_set = false, scissor_set = false;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkBuffer vertex_buffers[16] = {};
    VkDeviceSize vertex_offsets[16] = {};
    uint32_t vertex_count = 0;
    VkDescriptorSet constants_set = VK_NULL_HANDLE;
    uint32_t constants_offsets[3] = {};
  } bound_;
};

}  // namespace rex::graphics::sdnx
