/**
 * Sleeping Dogs port (SDNX): the GPU backend of the native renderer.
 *
 * NativeCommandProcessor decides everything a Xenos draw, resolve or swap needs (render targets in EDRAM-lite,
 * shaders from the library, vertex and index copies, constants, textures and samplers, barriers between passes);
 * the backend only records and submits it: Vulkan on the PC bench and on NVK, deko3d on the Switch.
 *
 * Plain values: formats, blend factors, compare functions, stencil operations, topologies and filters keep their
 * Vulkan enum values (vulkan_core.h is a header of plain C enums; the deko3d backend maps them to its own, no
 * Vulkan library is linked there). Objects the frontend holds are opaque backend handles.
 *
 * Descriptors: both backends have the same model, a table of image descriptors (one per heap: 2D, 3D, cube) and a
 * table of samplers, indexed by the numbers the frontend allocates and writes into the shared constant block
 * (BindTextures). Vulkan: bindless descriptor sets 0-3. deko3d: the image and sampler descriptor sets, and each
 * shader's texture bindings (sdnx_dksh/textures.txt) are bound from the shared block at draw time.
 *
 * Frames: kFramesInFlight command buffers / work slots with a fence each. Everything is recorded between
 * BeginFrame and SubmitFrame of one slot; objects that recorded commands may still use are released by the frontend
 * when the slot comes round again (BeginFrame has waited for its fence).
 */
#pragma once

#include <cstdint>

// As rex/ui/vulkan/api.h (which may be included after this): its vulkan.hpp needs the beta extensions declared.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan_core.h>

namespace rex::graphics::sdnx {

struct GpuImage;   // an image and its default view (render target, resolve destination, texture)
struct GpuView;    // a texture view: type, format, levels, layers, swizzle
struct GpuBuffer;  // a buffer: a frame's upload ring, the geometry caches, readbacks
struct GpuShader;  // a library shader: SPIR-V module (Vulkan) or DKSH (deko3d)

enum class ImageType : uint8_t { k2D, k3D, kCube };

struct ImageDesc {
  ImageType type = ImageType::k2D;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 1, height = 1;
  uint32_t depth = 1;   // 3D: slices
  uint32_t levels = 1;  // mips
  uint32_t layers = 1;  // 6 for a cube
  bool render_target = false;  // colour attachment
  bool depth_stencil = false;  // depth / stencil attachment (format D32_SFLOAT_S8_UINT)
  bool mutable_format = false; // views of another format of the same size (SNORM views of UNORM textures)
};

enum class BufferUsage : uint8_t {
  kUpload,    // CPU-written each frame: vertices, indices, constants, texture data, pulled vertices
  kGeometry,  // persistent, CPU-written (geometry caches)
  kReadback,  // GPU-written, CPU-read after the frame's fence (dumps, queries, brightness)
  kDeviceLocal,  // GPU memory filled by copies (the page-watched geometry cache): no mapping
};

struct BufferRef {
  GpuBuffer* buffer = nullptr;
  uint64_t offset = 0;
};

struct ImageRegion {
  int32_t x = 0, y = 0, z = 0;
  uint32_t width = 0, height = 0, depth = 1;
  uint32_t level = 0, layer = 0, layer_count = 1;
};

enum class Aspect : uint8_t { kColor = 1, kDepth = 2, kStencil = 4, kDepthStencil = 6 };

struct SamplerDesc {
  bool mag_linear = false, min_linear = false, mip_linear = false;
  bool base_map_only = false;  // max LOD 0.25
  VkSamplerAddressMode address[3] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                     VK_SAMPLER_ADDRESS_MODE_REPEAT};
  float lod_bias = 0.0f;
  VkBorderColor border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
};

struct ViewDesc {
  ImageType type = ImageType::k2D;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t levels = 1, layers = 1;
  VkComponentSwizzle swizzle[4] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                   VK_COMPONENT_SWIZZLE_A};
};

// One draw, fully decided by the frontend. The Vulkan backend turns the fixed part into a pipeline (cached by its
// hash) and records only the dynamic state that changed; deko3d binds each piece of state that changed.
struct DrawPacket {
  GpuShader* vs = nullptr;
  GpuShader* ps = nullptr;  // null: depth / stencil only
  uint32_t spec = 0;        // Vulkan specialization constant 0; deko3d: written into the shared block (byte 616)

  // Vertex input (Vulkan formats, offsets in bytes).
  struct Attribute {
    uint32_t location, binding;
    VkFormat format;
    uint32_t offset;
  };
  uint32_t attribute_count = 0, binding_count = 0;
  Attribute attributes[16];
  uint32_t strides[16] = {};
  BufferRef vertex_buffers[16];

  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  bool primitive_restart = false;

  // The targets of the open rendering pass (BeginRendering), for their formats; colors[i] may be null.
  GpuImage* colors[4] = {};
  uint32_t color_count = 0;
  GpuImage* depth = nullptr;

  // Fixed state: raw Xenos registers where the backends map them the same way.
  uint32_t blend_control[4] = {};  // RB_BLENDCONTROL per target
  uint32_t color_mask = 0;         // 4 bits per target, already reduced to the shader's exports and bound targets
  bool depth_test = false, depth_write = false;
  VkCompareOp depth_func = VK_COMPARE_OP_ALWAYS;
  uint32_t stencil = 0;  // RB_DEPTHCONTROL stencil bits (0: no stencil test)
  uint32_t cull = 0;     // bit 0 cull front, bit 1 cull back, bit 2 front face clockwise

  // Dynamic state.
  VkViewport viewport = {};
  VkRect2D scissor = {};
  float blend_constants[4] = {};
  uint32_t stencil_values[6] = {};  // reference front / back, compare mask front / back, write mask front / back
  float depth_bias_constant = 0.0f, depth_bias_slope = 0.0f;

  // Constants: VS (4096 bytes), PS (3584), shared block (944) in the frame's upload ring.
  BufferRef constants[3];
  uint32_t constant_sizes[3] = {};
  // The shared block as written (texture and sampler descriptor numbers per Xenos slot): deko3d binds each shader's
  // texture bindings from it. 236 words.
  const uint32_t* shared = nullptr;

  // Draw: indexed when index_count is not 0 (32-bit indices, restart 0xFFFFFFFF).
  BufferRef index_buffer;
  uint32_t index_count = 0;
  int32_t vertex_offset = 0;
  uint32_t vertex_count = 0;  // non-indexed draws
};

class GpuBackend {
 public:
  static constexpr uint32_t kFramesInFlight = 3;
  static constexpr uint32_t kHeapSize = 4096;  // image descriptors per heap, samplers

  virtual ~GpuBackend() = default;

  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;  // waits for the GPU first
  virtual const char* name() const = 0;
  // The shader package folder next to the executable and its file extension (the index is the same for both).
  virtual const char* ShaderPackage() const = 0;
  virtual const char* ShaderExtension() const = 0;
  // Bytes of a buffer the pulling vertex shaders can read through storage buffer 3 (from its start).
  virtual uint64_t MaxStorageBufferRange() const = 0;
  // Statistics: pipelines (or state objects) made so far.
  virtual uint64_t pipelines_created() const { return 0; }

  // ---- Frames
  // Waits for the slot's fence and starts recording into it.
  virtual void BeginFrame(uint32_t slot) = 0;
  virtual bool SubmitFrame(uint32_t slot) = 0;
  virtual void WaitIdle() = 0;

  // ---- Memory
  virtual GpuBuffer* CreateBuffer(uint64_t size, BufferUsage usage, uint8_t** mapping) = 0;
  virtual void DestroyBuffer(GpuBuffer* buffer) = 0;
  // The minimum alignment of a constant block in a buffer.
  virtual uint64_t ConstantAlignment() const = 0;

  // ---- Images and views
  virtual GpuImage* CreateImage(const ImageDesc& desc) = 0;
  virtual void DestroyImage(GpuImage* image) = 0;
  // New memory has undefined contents: colour 0, depth 1 / stencil 0 (and the layout Vulkan wants). clear = false
  // for images whose every texel is written next (texture uploads): only the layout.
  virtual void InitializeImage(GpuImage* image, const ImageDesc& desc, bool clear = true) = 0;
  virtual GpuView* CreateView(GpuImage* image, const ViewDesc& desc) = 0;
  virtual void DestroyView(GpuView* view) = 0;
  virtual bool FormatFilterable(VkFormat format) = 0;

  // ---- Descriptors (see the top of this file)
  virtual void WriteImageDescriptor(ImageType heap, uint32_t index, GpuView* view) = 0;
  virtual void WriteSamplerDescriptor(uint32_t index, const SamplerDesc& desc) = 0;

  // ---- Transfers (outside rendering passes; the frontend ends the pass first). Clears cover whole subresources
  // (region.level, region.layer .. + layer_count); its rectangle is ignored.
  virtual void ClearColor(GpuImage* image, const ImageRegion& region, const float rgba[4]) = 0;
  virtual void ClearDepthStencil(GpuImage* image, const ImageRegion& region, Aspect aspects, float depth,
                                 uint32_t stencil) = 0;
  virtual void CopyImage(GpuImage* source, const ImageRegion& from, GpuImage* destination, const ImageRegion& to,
                         Aspect aspect) = 0;
  // Scaled copy (MSAA reduction: nearest, one sample per pixel).
  virtual void BlitImage(GpuImage* source, const ImageRegion& from, GpuImage* destination, const ImageRegion& to,
                         Aspect aspect, bool linear) = 0;
  // row_length / image_height in texels (0: tightly packed).
  virtual void CopyBufferToImage(BufferRef source, uint32_t row_length, uint32_t image_height,
                                 GpuImage* destination, const ImageRegion& to, Aspect aspect) = 0;
  virtual void CopyImageToBuffer(GpuImage* source, const ImageRegion& from, Aspect aspect, BufferRef destination,
                                 uint32_t row_length) = 0;
  virtual void CopyBuffer(BufferRef source, BufferRef destination, uint64_t size) = 0;

  // ---- Ordering. Full: everything before, before everything after. Attachments: only attachment writes of earlier
  // passes before attachment access of later ones.
  virtual void FullBarrier() = 0;
  virtual void AttachmentBarrier() = 0;

  // ---- Draws. The frontend opens and ends rendering passes (it decides the barriers between them); draws are
  // recorded into the open pass. Constants: packet.constants are blocks of one buffer (the frame's upload ring), whose
  // start is also storage buffer 3 for the pulling vertex shaders.
  // `name`: the shader's base name in the package (deko3d finds its texture bindings by it).
  virtual bool CreateShader(GpuShader*& shader, bool vertex, const void* code, uint64_t size, const char* name) = 0;
  virtual void DestroyShader(GpuShader* shader) = 0;
  virtual void BeginRendering(GpuImage* const colors[4], uint32_t color_count, GpuImage* depth, uint32_t width,
                              uint32_t height) = 0;
  // false: not drawn (logged once by the backend).
  virtual bool Draw(const DrawPacket& packet) = 0;
  virtual void EndRendering() = 0;
  // Fills a rectangle of a colour target as an attachment (EDRAM aliasing of depth clears).
  virtual void ClearAttachmentRect(GpuImage* target, const VkRect2D& rect, const float rgba[4]) = 0;

  // ---- Occlusion queries: one host query per rendering pass instance a guest query spans.
  virtual bool BeginQuery(uint32_t slot, uint32_t index) = 0;  // inside the open pass
  virtual void EndQuery(uint32_t slot, uint32_t index) = 0;
  virtual void ResetQueries(uint32_t slot, uint32_t count) = 0;
  // Before the slot's submit: the sample counts of queries 0..count-1 into its readback, 64 bits each.
  virtual void ResolveQueries(uint32_t slot, uint32_t count) = 0;
  virtual const uint64_t* QueryResults(uint32_t slot) = 0;  // after the slot's fence

  // ---- Output: the front image scaled into the presenter's guest output (width x height); submits the frame.
  // false: not presented (the frontend submits the frame itself).
  virtual bool Present(uint32_t slot, GpuImage* front, uint32_t front_width, uint32_t front_height, uint32_t width,
                       uint32_t height, bool is_8bpc) = 0;
};

}  // namespace rex::graphics::sdnx
