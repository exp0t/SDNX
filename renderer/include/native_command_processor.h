/**
 * Sleeping Dogs port (SDNX): native renderer command processor (stage 2b: EDRAM-lite, resolves, output).
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::sdnx {

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  // Registers from 0x2000 up (context state, ALU / fetch / bool / loop constants) have no side effects in the base
  // WriteRegister (its special cases are all below 0x2000): stored directly, without its per-register lookup.
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;
  bool ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet, uint32_t count) override;

 private:
  static constexpr uint32_t kFramesInFlight = 3;

  // A host image (all kept in VK_IMAGE_LAYOUT_GENERAL in this stage).
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0;
    bool initialized = false;  // transitioned from UNDEFINED to GENERAL
    bool drawn = false;        // some draw targeted this image (diagnostic)
  };

  // EDRAM-lite: one host image per (base, pitch, format, MSAA, depth); pixel (x, y) of the surface is at
  // (x, y) of the image (MSAA kept at one sample per pixel in this stage).
  static uint64_t RenderTargetKey(uint32_t base_tiles, uint32_t pitch_pixels, uint32_t format,
                                  xenos::MsaaSamples msaa, bool is_depth);

  const ui::vulkan::VulkanDevice* vulkan_device() const;
  bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, bool depth);
  void DestroyImage(Image& image);
  // Image in GENERAL layout, at least width x height (recreated bigger if needed, contents lost).
  Image* GetRenderTarget(uint32_t base_tiles, uint32_t pitch_pixels, uint32_t format, xenos::MsaaSamples msaa,
                         bool is_depth, uint32_t min_height);
  Image* GetResolved(uint32_t address, VkFormat format, uint32_t width, uint32_t height);
  void InitializeLayout(Image& image);
  // Images replaced while the current frame may still reference them are destroyed once that frame's
  // fence has passed (destroying them at once is a use-after-free in the command buffer).
  void RetireImage(Image& image);
  std::array<std::vector<Image>, kFramesInFlight> retired_images_;
  bool dummies_initialized_ = false;
  void FullBarrier();
  // Depth resolve (k_24_8 / k_24_8_FLOAT textures): the depth aspect of the render target goes through the
  // upload buffer into an R32_SFLOAT resolved image (the [0, 1] depth, as the guest texture reads it); MSAA
  // targets (in samples) are reduced with a nearest blit, which picks one sample per pixel.
  void ResolveDepth(const Image& source, const draw_util::ResolveInfo& info, uint32_t x0, uint32_t y0,
                    uint32_t width, uint32_t height, uint32_t sx, uint32_t sy);
  Image depth_resolve_temp_;
  // Stencil of depth resolves (R8), by destination base: needed to restore the depth buffer from the texture.
  std::unordered_map<uint32_t, Image> resolved_stencil_;
  // The game restores depth / stencil after the tiled G-buffer by drawing the resolved depth texture as
  // k_8_8_8_8 colour into the EDRAM range of the depth buffer (colour and depth alias there). Colour and depth
  // are separate images here, so that draw is replaced by a copy into the depth image. True if handled.
  bool TryRestoreDepth(Shader* ps);

  void EnsureFrame();
  bool SubmitFrame();
  bool CreatePresentPipeline();
  void ReportStats();

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kFramesInFlight> command_buffers_{};
  std::array<VkFence, kFramesInFlight> fences_{};
  uint64_t frame_ = 0;
  bool frame_open_ = false;
  VkCommandBuffer cb_ = VK_NULL_HANDLE;

  std::unordered_map<uint64_t, Image> render_targets_;
  std::unordered_map<uint32_t, Image> resolved_;  // by physical destination base

  // Output: compute copy of the front buffer into the guest output image.
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout present_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool present_descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kFramesInFlight> present_sets_{};
  VkSampler linear_sampler_ = VK_NULL_HANDLE;

  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;

  // ---- Draws (native_draw.cpp) ----
  struct FetchSlot {
    uint16_t address;   // container instruction address of vertex fetch K
    uint8_t reg, mask;  // destination register, written components
    uint8_t location;   // vertex input location (0xFF: none)
  };
  struct LibraryEntry {
    std::string name;
    bool vertex = false;
    std::vector<FetchSlot> fetches;
    VkShaderModule module = VK_NULL_HANDLE;
    bool load_failed = false;
    // "pull" in the index line: the shader reads its vertices itself by a computed index (hand-made instancing),
    // from the upload buffer (set 4 binding 3) with the per-fetch offset / stride / format of the shared block.
    bool pull = false;
  };
  struct VertexFetchRuntime {
    uint32_t address;  // run-time instruction address
    uint32_t w0, w1, w2;
  };
  struct VertexShaderInfo {
    uint64_t masked_key = 0;
    std::vector<VertexFetchRuntime> fetches;
  };
  struct UploadBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapping = nullptr;
    VkDeviceSize size = 0, offset = 0;
  };
  // 64 MB overflowed in gameplay (22-42 MB of vertex copies + ~20 MB of constants per frame): draws were skipped.
  // TODO: back to 64 MB or less once the geometry cache and the constant uploads are efficient (Switch memory).
#ifdef __SWITCH__
  // Switch: 3 x 128 MB (a gameplay frame takes ~40 MB: ~14 MB of vertex copies + constants).
  static constexpr VkDeviceSize kUploadBufferSize = VkDeviceSize(128) << 20;
#else
  static constexpr VkDeviceSize kUploadBufferSize = VkDeviceSize(256) << 20;
#endif
  // 688 + 16 vertex fetches x {byte offset in the upload buffer, stride in bytes, VkFormat, vertex count} for the
  // shaders that pull their vertices (bytes 688-943, see sdnxPullFetch in shader_common.h).
  static constexpr uint32_t kSharedBlockSize = 944;
  static constexpr uint32_t kPullInfoOffset = 688;
  VkDeviceSize pull_range_ = 0;  // bytes of the upload buffer the pulling shaders can read (set 4 binding 3)

  bool InitializeDraws();
  void ShutdownDraws();
  bool LoadLibrary();
  LibraryEntry* FindVertexShader(Shader* shader, const VertexShaderInfo*& info);
  LibraryEntry* FindPixelShader(Shader* shader);
  VkShaderModule GetModule(LibraryEntry& entry);
  // Bytes in the frame's upload buffer; returns the offset or UINT64_MAX when full.
  VkDeviceSize Upload(const void* data, VkDeviceSize size, VkDeviceSize alignment, uint8_t** mapping_out = nullptr);
  void EndRendering();
  bool DrawNative(xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info);

  std::unordered_map<uint64_t, LibraryEntry> library_vs_, library_ps_;
  std::unordered_map<const Shader*, VertexShaderInfo> vs_info_;
  std::array<UploadBuffer, kFramesInFlight> upload_{};
  VkDescriptorSetLayout heap_layouts_[4] = {};  // Texture2D, Texture3D, TextureCube, samplers (sets 0-3)
  VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;  // set 4: 3 dynamic UBOs
  VkDescriptorPool draw_descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet heap_sets_[4] = {};
  std::array<VkDescriptorSet, kFramesInFlight> constants_sets_{};
  VkPipelineLayout draw_pipeline_layout_ = VK_NULL_HANDLE;
  Image dummy_2d_, dummy_3d_, dummy_cube_;
  VkSampler dummy_sampler_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  // Current dynamic rendering pass.
  bool rendering_ = false;
  uint64_t rendering_key_ = 0;

  // ---- Textures (native_texture.cpp) ----
  // A guest texture loaded into a host image: all its stored levels (base and mips), 2D or cube. Reloaded when
  // the guest writes to its pages (physical memory write watch, per-page version counters).
  struct Texture {
    Image image;
    uint32_t levels = 1, layers = 1, depth = 1;
    uint32_t page_first = 0, page_count = 0;  // base pages
    uint32_t mip_page_first = 0, mip_page_count = 0;
    uint64_t version = 0;              // sum of the page versions when it was loaded
    uint64_t validated_frame = ~0ull;  // frame of the last version check
    bool loaded = false, failed = false;
    // Cube maps the game renders (6 faces resolved to its memory, which the native renderer never writes):
    // level 0 of each face is copied from the resolved images (see OverlayResolvedFaces).
    uint64_t face_generation = ~0ull;
    bool faces_resolved = false, faces_swapped = false;
    // XXH3 of the guest pages it was loaded from: a write elsewhere in those pages changes the version without
    // changing the texture (~4 needless reloads a frame in gameplay, 12 % of the GPU thread on the console).
    uint64_t content_hash = 0;
  };
  uint64_t TextureContentHash(const Texture& texture);
  std::vector<uint8_t> texture_scratch_;  // LoadTexture: the byte-swapped guest data of one slice
  // Graphics state already recorded in the current command buffer (it survives rendering passes, copies and
  // clears; the present uses the compute bind point): only changes are recorded. Each vkCmdSet* costs CPU time in
  // the driver (NVK copies and flushes its dynamic state). Reset by vkBeginCommandBuffer.
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
  } bound_;
  // Format of the view GetTextureDescriptor chose last (for the sampler: no linear filter where unsupported).
  VkFormat last_texture_format_ = VK_FORMAT_UNDEFINED;
  std::unordered_map<int, bool> filterable_formats_;
  bool FormatFilterable(VkFormat format);
  // Copies the resolved images of the faces of a cube texture into it; true if the cube comes from resolves.
  bool OverlayResolvedFaces(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch);
  struct TextureView {
    VkImageView view = VK_NULL_HANDLE;
    uint32_t heap = 0, index = 0;  // heap 0 2D, 1 3D, 2 cube
  };
  void InitializeTextures();
  void ShutdownTextures();
  // Fills the texture and sampler parts of the shared block for the textures the shaders of the draw use.
  // May end the rendering pass (texture uploads are transfers).
  void BindTextures(Shader* vs, Shader* ps, uint32_t* shared);
  // Heap index (with the sign bits) of the view of a texture for a fetch constant, 0 for the placeholder.
  uint32_t GetTextureDescriptor(uint32_t fetch_constant, uint32_t& heap, float inv_size[2]);
  uint32_t GetSamplerDescriptor(const xenos::xe_gpu_texture_fetch_t& fetch,
                                const ParsedTextureFetchInstruction& instr);
  bool LoadTexture(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch);
  uint64_t TexturePagesVersion(const Texture& texture) const;
  // view_format: VK_FORMAT_UNDEFINED for the image format (an SNORM view of a mutable-format texture otherwise).
  uint32_t GetView(const Image& image, uint32_t heap, uint32_t levels, uint32_t layers, uint32_t swizzle,
                   uint32_t host_swizzle, VkFormat view_format = VK_FORMAT_UNDEFINED);
  // Views of an image being retired go with it (their heap indices are reused after the frame completes).
  void RetireViews(VkImage image);
  void ReleaseRetiredViews(uint32_t slot);
  static std::pair<uint32_t, uint32_t> PhysicalWriteCallback(void* context, uint32_t start, uint32_t length,
                                                            bool exact);

  // Resolved images are allocated with the destination pitch alignment (32), textures read them with their exact
  // size: a texture-sized copy keeps the normalized coordinates right (the menu blur chain came out striped).
  // Also: the game renders the 1200x704 scene as two 1200x352 halves (the G-buffer does not fit the EDRAM) and
  // resolves the second half to the address of row 352 of the same texture - the copy is assembled from every
  // resolve that landed inside the texture's memory range (parts).
  struct ResolvedPart {
    uint32_t address, x, y;
  };
  struct ResolvedCopy {
    Image image;
    uint64_t generation = ~0ull;
    std::vector<ResolvedPart> parts;
    size_t parts_resolved_count = ~size_t(0);  // resolved_.size() when the parts were found
    uint64_t parts_frame = ~0ull;              // frame they were found in
  };
  std::unordered_map<uint64_t, ResolvedCopy> resolved_copies_;
  std::unordered_map<uint32_t, uint64_t> resolved_generation_;
  // RB_COPY_DEST_INFO.copy_dest_swap of the last colour resolve to that address: Xenos then writes red and blue
  // swapped. The image keeps them unswapped; texture views swap them back (the AO / shadow collector of the scene
  // composite is resolved that way, its .z read 0 and the whole frame came out black - 2026-11-12).
  std::unordered_map<uint32_t, bool> resolved_swap_;
  // Frame of the last resolve to each address: a texture assembled from several resolves (ResolvedCopy.parts)
  // takes only parts as fresh as its base. resolved_ keeps images from long ago (menus, other scenes); one of them
  // inside the scene texture's range was pasted over the right 1/16 of the frame (lighter strip, 2026-11-12).
  std::unordered_map<uint32_t, uint64_t> resolved_frame_;
  std::unordered_map<uint32_t, uint32_t> resolved_extent_;  // destination base -> bytes the resolve covers  // by destination base, bumped by every resolve
  std::unordered_map<uint64_t, Texture> textures_;
  std::unordered_map<uint64_t, TextureView> views_;                  // (image, heap, swizzle) -> view
  std::unordered_map<VkImage, std::vector<uint64_t>> image_views_;  // image -> keys in views_
  std::array<std::vector<TextureView>, kFramesInFlight> retired_views_;
  std::vector<uint32_t> free_indices_[3];
  uint32_t next_index_[3] = {1, 1, 1};
  std::unordered_map<uint64_t, uint32_t> samplers_;  // key -> sampler heap index
  std::vector<VkSampler> sampler_objects_;
  std::unique_ptr<std::atomic<uint32_t>[]> page_versions_;
  void* write_watch_handle_ = nullptr;
  std::unordered_map<uint32_t, uint32_t> unsupported_formats_;

  // ---- Vertex / index cache (native_geometry.cpp) ----
  bool InitializeGeometry();
  void ShutdownGeometry();
  // Offset in geometry_buffer_ of the host copy of guest memory [address, address + size) (fill writes host_size
  // bytes of it); UINT64_MAX when the caller has to upload it for this draw (dynamic data, cache full).
  // A non-owning reference to the fill callback: std::function allocated the lambda's captures on the heap on
  // every call (each binding and index buffer of every draw; malloc/free were 16 % of the GPU thread on Switch).
  class FillRef {
   public:
    template <typename F>
    FillRef(F&& f) : object_(const_cast<void*>(static_cast<const void*>(&f))),
                     call_([](void* object, uint8_t* out) { (*static_cast<std::remove_reference_t<F>*>(object))(out); }) {}
    void operator()(uint8_t* out) const { call_(object_, out); }

   private:
    void* object_;
    void (*call_)(void*, uint8_t*);
  };
  VkDeviceSize CachedGeometry(uint64_t key, uint32_t address, uint32_t size, VkDeviceSize host_size, FillRef fill);
  VkDeviceSize AllocateGeometry(VkDeviceSize size);
  void FreeGeometry(VkDeviceSize offset, VkDeviceSize size);
  void ReleaseRetiredGeometry(uint32_t slot);  // also evicts entries unused for a long time
  uint64_t PagesVersion(uint32_t first, uint32_t count) const;
  struct GeometryEntry {
    VkDeviceSize offset = UINT64_MAX, size = 0;
    uint32_t page_first = 0, page_count = 0;
    uint64_t version = 0, epoch = 0, last_change_frame = 0, dynamic_since = 0;
    uint64_t first_seen = 0, last_used = 0;
    uint32_t changes = 0;
    bool dynamic = false;
    uint64_t hot_until = 0;  // refused for hot pages: no page scan again before this frame (content hash path)
  };
  VkBuffer geometry_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory geometry_memory_ = VK_NULL_HANDLE;
  std::map<VkDeviceSize, VkDeviceSize> geometry_free_;  // offset -> size
  std::array<std::vector<std::pair<VkDeviceSize, VkDeviceSize>>, kFramesInFlight> geometry_retired_;
  std::unordered_map<uint64_t, GeometryEntry> geometry_;
  std::atomic<uint64_t> write_epoch_{0};  // bumped by every physical write callback
  // Frame (low 32 bits, 0 = never) of the last write the callbacks saw on each page: geometry on pages written
  // recently is dynamic (ring buffers) and is not cached or watched.
  std::unique_ptr<std::atomic<uint32_t>[]> page_write_frame_;
  std::atomic<uint32_t> callback_frame_{1};
  // Watched writes seen on the page (saturating): the hot period doubles with each one, because writes to pages
  // that are not watched are invisible - without it a ring buffer page was cached again every 120 frames.
  std::unique_ptr<std::atomic<uint8_t>[]> page_strikes_;
  std::atomic<uint64_t> write_callbacks_{0};
  uint64_t write_callbacks_reported_ = 0;
  bool PagesHot(uint32_t first, uint32_t count) const;

  // Content-hashed copies (second level, for what the page-watched cache refuses: data on pages written recently).
  // Most of a gameplay frame's geometry sat on such pages and was copied and converted again on every draw
  // (vertices + indices ~55 % of the draw's CPU). Key: the guest range and its format; value: XXH3 of the guest bytes
  // and the converted copy in a persistent host-visible buffer (written by the CPU directly: no copy commands, no
  // break of the rendering pass). For indices also their min / max (no scan for the vertex range).
  struct HashedGeometry {
    uint64_t content = 0;
    VkDeviceSize offset = UINT64_MAX, size = 0;
    uint64_t last_used = 0;
    uint32_t index_min = 0, index_max = 0;
    uint64_t hashed_frame = UINT64_MAX;  // last frame its content was hashed (and matched or was filled)
    uint64_t content_since = 0;          // frame the current content was filled
  };
  VkBuffer hashed_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory hashed_memory_ = VK_NULL_HANDLE;
  uint8_t* hashed_mapping_ = nullptr;
  std::map<VkDeviceSize, VkDeviceSize> hashed_free_;
  std::array<std::vector<std::pair<VkDeviceSize, VkDeviceSize>>, kFramesInFlight> hashed_retired_;
  std::unordered_map<uint64_t, HashedGeometry> hashed_;
  // The entry of `key` holding `content` (allocated and filled by `fill` when absent or different), or nullptr.
  HashedGeometry* HashedCopy(uint64_t key, uint64_t content, VkDeviceSize host_size, FillRef fill);
  // Same frame, stable content: the entry without hashing again (nullptr: hash and call HashedCopy).
  HashedGeometry* HashedReuse(uint64_t key, VkDeviceSize host_size);

  // ---- Occlusion queries (native_query.cpp) ----
  // Without stalls: the guest gets the last known count of that sample-count address at once (1000 = visible
  // before the first one), the real count is read back when the frame's fence has passed (1-3 frames later).
  // A guest query is a host query per rendering pass instance it spans (Vulkan queries cannot cross passes).
  static constexpr uint32_t kMaxQueries = 4096;
  bool InitializeQueries();
  void ShutdownQueries();
  void BeginHostQuery();  // inside a rendering pass, while a guest query is active
  void EndHostQuery();
  void ResetQueries(uint32_t slot);      // after the slot's fence: results to query_results_, pool reset
  void CopyQueryResults(uint32_t slot);  // before the frame's submit
  std::array<VkQueryPool, kFramesInFlight> query_pools_{};
  std::array<VkBuffer, kFramesInFlight> query_readback_{};
  std::array<VkDeviceMemory, kFramesInFlight> query_readback_memory_{};
  std::array<const uint64_t*, kFramesInFlight> query_readback_mapping_{};
  std::array<uint32_t, kFramesInFlight> query_used_{};
  struct PendingQuery {
    uint32_t address;
    std::vector<uint32_t> indices;
  };
  std::array<std::vector<PendingQuery>, kFramesInFlight> pending_queries_;
  bool guest_query_active_ = false, host_query_open_ = false;
  uint32_t guest_query_address_ = 0;
  std::vector<uint32_t> guest_query_indices_;             // in the current frame's pool
  std::unordered_map<uint32_t, uint32_t> query_results_;  // end sample-count address -> samples

  struct Stats {
    uint64_t swaps = 0, draws = 0, indices = 0, copies = 0, copies_depth = 0, clears = 0, shader_loads = 0,
             shader_new = 0, front_found = 0, drawn = 0, missing_vs = 0, missing_ps = 0, skipped = 0,
             new_pipelines = 0, copies_incompatible = 0, pulled = 0;
    uint64_t tex_loads = 0, tex_bytes = 0, tex_resolved = 0, tex_unsupported = 0, tex_signed = 0;
    uint64_t upload_full = 0, geo_first = 0, geo_hot = 0, geo_flagged = 0, geo_changed = 0;
    uint64_t geo_hits = 0, geo_uploads = 0, geo_bytes = 0, geo_dynamic = 0, geo_direct_bytes = 0;
    uint64_t hash_hits = 0, hash_fills = 0, hash_fill_bytes = 0, hash_bytes = 0, hash_whole_bytes = 0, hash_reuse = 0; uint64_t constants_reused = 0, constants_bind_skipped = 0; uint64_t prof_sub[3] = {};
    mutable uint64_t hot_checks = 0, hot_age = 0, hot_strikes = 0, hot_strikes_max = 0;  // PagesHot refusals (SDNX_PROF)  // content-hashed geometry copies
    uint64_t prof_ns[6] = {}, prof_draws = 0;  // SDNX_PROF: time of the parts of DrawNative (steady_clock ticks)
  } stats_;

  // Diagnostic front-buffer dump (SDNX_DUMP_FRONT=N): copies the presented image to a staging buffer once
  // per swap for N swaps, logs per-channel statistics, and writes a PPM next to the shader package.
  void BeginFrontDump(const Image& front);
  void FinishFrontDump();
  // SDNX_DUMP_NTH=N with SDNX_DUMP_ADDR / SDNX_DUMP_AT: dump the N-th resolve to that address within the frame,
  // at the moment of the resolve (an address is often resolved several times a frame).
  void DumpResolveIfRequested(uint32_t dest_base, const Image& dest);
  bool dump_nth_frame_ = false;
  uint32_t dump_nth_count_ = 0;
  VkBuffer dump_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory dump_memory_ = VK_NULL_HANDLE;
  void* dump_mapping_ = nullptr;
  VkDeviceSize dump_size_ = 0;
  uint32_t dump_width_ = 0, dump_height_ = 0, dump_bpp_ = 0;
  uint32_t front_address_ = 0, dump_address_ = 0;
  Image* last_color_rt_ = nullptr;  // diagnostic: the colour target of the last draw
  int dump_remaining_ = 0;
  bool dump_pending_ = false;
  uint32_t dump_index_ = 0;
  std::chrono::steady_clock::time_point stats_start_;
  // Diagnostic: SDNX_LOG_FRAME=N logs every draw, resolve and clear of swap N (counted from the first swap).
  uint64_t swap_count_ = 0;
  bool log_frame_ = false;
  uint32_t log_draw_ = 0;
};

}  // namespace rex::graphics::sdnx



