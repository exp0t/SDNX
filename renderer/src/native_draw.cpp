/**
 * Sleeping Dogs port (SDNX): native renderer, draws (stage 2b).
 *
 * Shaders come from the offline package (port/tools/shaderlib/build_shaderlib.sh -> exe/sdnx_shaders): pixel
 * shaders by the XXH3 of their microcode, vertex shaders by the XXH3 of the microcode with every vertex fetch
 * zeroed (Direct3D rewrites the fetches at bind). Direct3D's fetch words of the bound variant give the vertex
 * attributes and the run-time destination swizzles (g_FetchSwizzle). Vertex and index data are copied from guest
 * memory at the draw with the byte swap of their fetch constant / index endianness. Constants go to dynamic UBOs
 * (set 4, MASSEFFECT_UBO); textures and samplers go to the bindless sets 0-3 (native_texture.cpp).
 */
#include <rex/graphics/sdnx/native_command_processor.h>

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::sdnx {

namespace {

constexpr uint32_t kHeapSize = 8192;  // gameplay overflowed 1024 views (white placeholders)
constexpr uint32_t kSpecConstantsUbo = 1u << 8;
constexpr uint32_t kSpecAlphaTest = 1u << 1;

uint32_t ByteSwap32(uint32_t v) { return __builtin_bswap32(v); }

// Copies guest data with the swap of its Xenos endianness (as the vertex / index fetch sees it).
void CopySwapped(uint8_t* dst, const uint8_t* src, size_t size, xenos::Endian endian) {
  size_t words = size / 4;
  const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
  uint32_t* d = reinterpret_cast<uint32_t*>(dst);
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i < words; ++i) {
        uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i < words; ++i) d[i] = ByteSwap32(s[i]);
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i < words; ++i) d[i] = (s[i] >> 16) | (s[i] << 16);
      break;
    default:
      std::memcpy(dst, src, words * 4);
      break;
  }
  std::memcpy(dst + words * 4, src + words * 4, size - words * 4);
}

VkFormat VertexFormat(uint32_t format, bool is_signed, bool normalized) {
  auto pick = [&](VkFormat un, VkFormat sn, VkFormat us, VkFormat ss) {
    return normalized ? (is_signed ? sn : un) : (is_signed ? ss : us);
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED,
                  VK_FORMAT_R8G8B8A8_SSCALED);
    case 7:  // k_2_10_10_10
      return pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                  VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32);
    case 25:  // k_16_16
      return pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                  VK_FORMAT_R16G16_SSCALED);
    case 26:  // k_16_16_16_16
      return pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_USCALED,
                  VK_FORMAT_R16G16B16A16_SSCALED);
    case 31:
      return VK_FORMAT_R16G16_SFLOAT;
    case 32:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:
      return is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case 34:
      return is_signed ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
    case 35:
      return is_signed ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
    case 36:
      return VK_FORMAT_R32_SFLOAT;
    case 37:
      return VK_FORMAT_R32G32_SFLOAT;
    case 38:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 57:
      return VK_FORMAT_R32G32B32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

// A k_10_11_11 (format 16: x 11 bits, y 11, z 10 from bit 0) or k_11_11_10 (17: x 10, y 11, z 11) dword of every
// vertex of a buffer copy (host word order) repacked in place to 8:8:8:8, SNORM or UNORM, w = 1.
void RepackNormals(uint8_t* data, uint32_t size, uint32_t stride, uint32_t offset, uint32_t format, bool is_signed) {
  if (!stride) return;
  // Component value -> 8-bit code, for 10 and 11 bits, signed and unsigned (a few MB of normals a frame: no
  // floating point per vertex).
  static const auto tables = [] {
    std::array<std::array<uint8_t, 2048>, 4> t{};  // [signed * 2 + (bits == 11)]
    for (uint32_t s = 0; s < 2; ++s) {
      for (uint32_t eleven = 0; eleven < 2; ++eleven) {
        uint32_t n = eleven ? 11 : 10;
        for (uint32_t raw = 0; raw < (1u << n); ++raw) {
          uint32_t q;
          if (s) {
            int32_t v = int32_t(raw << (32 - n)) >> (32 - n);
            float f = std::max(float(v) / float((1 << (n - 1)) - 1), -1.0f);
            q = uint32_t(int32_t(std::lround(f * 127.0f))) & 0xFF;
          } else {
            q = uint32_t(std::lround(float(raw) / float((1u << n) - 1) * 255.0f));
          }
          t[s * 2 + eleven][raw] = uint8_t(q);
        }
      }
    }
    return t;
  }();
  const uint32_t bits[3] = {format == 16 ? 11u : 10u, 11u, format == 16 ? 10u : 11u};
  const uint8_t* lut[3] = {tables[(is_signed ? 2 : 0) + (bits[0] == 11)].data(),
                           tables[(is_signed ? 2 : 0) + 1].data(),
                           tables[(is_signed ? 2 : 0) + (bits[2] == 11)].data()};
  const uint32_t w = (is_signed ? 127u : 255u) << 24;
  for (uint32_t at = offset; at + 4 <= size; at += stride) {
    uint32_t v;
    std::memcpy(&v, data + at, 4);
    uint32_t out = w | lut[0][v & ((1u << bits[0]) - 1)] | uint32_t(lut[1][(v >> bits[0]) & 0x7FF]) << 8 |
                   uint32_t(lut[2][v >> (bits[0] + 11)]) << 16;
    std::memcpy(data + at, &out, 4);
  }
}

VkBlendFactor BlendFactor(xenos::BlendFactor f) {
  switch (uint32_t(f)) {
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

VkBlendOp BlendOp(xenos::BlendOp op) {
  switch (uint32_t(op)) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

uint32_t SwizzleMask(uint32_t swizzle) {
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (3 * i)) & 7) < 6) mask |= 1u << i;
  }
  return mask;
}

// Run-time vertex fetches (host-order words) and the masked key, like port/tools/shaderlib/ucode_hash.c.
void AnalyzeVertexShader(const std::vector<uint32_t>& w, uint64_t& masked_key,
                         std::vector<uint32_t>& fetch_addresses) {
  std::vector<uint32_t> be(w.size());
  for (size_t i = 0; i < w.size(); ++i) be[i] = ByteSwap32(w[i]);
  uint32_t first_instr = UINT32_MAX;
  uint32_t words = uint32_t(w.size());
  for (uint32_t i = 0;; ++i) {
    if (first_instr != UINT32_MAX && i >= first_instr) break;
    if (i * 3 + 2 >= words) break;
    uint64_t cfs[2] = {uint64_t(w[i * 3]) | (uint64_t(w[i * 3 + 1] & 0xFFFF) << 32),
                       uint64_t(w[i * 3 + 1] >> 16) | (uint64_t(w[i * 3 + 2]) << 16)};
    for (uint64_t cf : cfs) {
      uint32_t op = uint32_t(cf >> 44) & 0xF;
      if (!(op >= 1 && op <= 6) && op != 13 && op != 14) continue;
      uint32_t addr = uint32_t(cf) & 0xFFF, count = uint32_t(cf >> 12) & 7, seq = uint32_t(cf >> 16) & 0xFFF;
      first_instr = std::min(first_instr, addr);
      for (uint32_t j = 0; j < count; ++j) {
        if (!((seq >> (2 * j)) & 1)) continue;
        uint32_t g = (addr + j) * 3;
        if (g + 2 < words && (w[g] & 0x1F) == 0) {
          be[g] = be[g + 1] = be[g + 2] = 0;
          fetch_addresses.push_back(addr + j);
        }
      }
    }
  }
  masked_key = XXH3_64bits(be.data(), be.size() * sizeof(uint32_t));
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Library.

bool NativeCommandProcessor::LoadLibrary() {
  std::filesystem::path dir;
  const char* env = std::getenv("SDNX_SHADERS");
  dir = (env && *env) ? std::filesystem::path(env) : rex::filesystem::GetExecutableFolder() / "sdnx_shaders";
  std::ifstream index(dir / "index.txt");
  if (!index) {
    REXGPU_ERROR("[sdnx_native] shader package not found: {}", (dir / "index.txt").string());
    return false;
  }
  std::string line;
  while (std::getline(index, line)) {
    std::istringstream ls(line);
    std::string kind, key_hex, name;
    if (!(ls >> kind >> key_hex >> name)) continue;
    LibraryEntry entry;
    entry.name = (dir / (name + ".spv")).string();
    entry.vertex = kind == "vs";
    std::string slot;
    while (ls >> slot) {
      if (slot == "pull") {
        entry.pull = true;
        continue;
      }
      unsigned a = 0, r = 0, m = 0, l = 0xFF;
      if (std::sscanf(slot.c_str(), "%u:%u:%u:%u", &a, &r, &m, &l) >= 3) {
        entry.fetches.push_back({uint16_t(a), uint8_t(r), uint8_t(m), uint8_t(l)});
      }
    }
    uint64_t key = std::strtoull(key_hex.c_str(), nullptr, 16);
    (entry.vertex ? library_vs_ : library_ps_).emplace(key, std::move(entry));
  }
  REXGPU_INFO("[sdnx_native] shader package: {} vertex, {} pixel shaders ({})", library_vs_.size(),
              library_ps_.size(), dir.string());
  return true;
}

VkShaderModule NativeCommandProcessor::GetModule(LibraryEntry& entry) {
  if (entry.module || entry.load_failed) return entry.module;
  std::ifstream f(entry.name, std::ios::binary);
  std::vector<uint32_t> code;
  if (f) {
    f.seekg(0, std::ios::end);
    size_t size = size_t(f.tellg());
    f.seekg(0);
    code.resize(size / 4);
    f.read(reinterpret_cast<char*>(code.data()), std::streamsize(code.size() * 4));
  }
  if (!code.empty()) {
    entry.module = ui::vulkan::util::CreateShaderModule(vulkan_device(), code.data(), code.size() * 4);
  }
  if (!entry.module) {
    entry.load_failed = true;
    REXGPU_ERROR("[sdnx_native] could not load {}", entry.name);
  }
  return entry.module;
}

NativeCommandProcessor::LibraryEntry* NativeCommandProcessor::FindVertexShader(Shader* shader,
                                                                               const VertexShaderInfo*& info_out) {
  auto it = vs_info_.find(shader);
  if (it == vs_info_.end()) {
    VertexShaderInfo info;
    std::vector<uint32_t> addresses;
    AnalyzeVertexShader(shader->ucode_data(), info.masked_key, addresses);
    const std::vector<uint32_t>& w = shader->ucode_data();
    for (uint32_t a : addresses) info.fetches.push_back({a, w[a * 3], w[a * 3 + 1], w[a * 3 + 2]});
    it = vs_info_.emplace(shader, std::move(info)).first;
  }
  info_out = &it->second;
  auto entry = library_vs_.find(it->second.masked_key);
  return entry != library_vs_.end() ? &entry->second : nullptr;
}

NativeCommandProcessor::LibraryEntry* NativeCommandProcessor::FindPixelShader(Shader* shader) {
  auto entry = library_ps_.find(shader->ucode_data_hash());
  return entry != library_ps_.end() ? &entry->second : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Resources.

bool NativeCommandProcessor::InitializeDraws() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkDevice vk = device->device();
  LoadLibrary();

  for (UploadBuffer& upload : upload_) {
    uint32_t memory_type;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            device, kUploadBufferSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, upload.buffer, upload.memory, &memory_type)) {
      return false;
    }
    void* mapping = nullptr;
    dfn.vkMapMemory(vk, upload.memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    upload.mapping = static_cast<uint8_t*>(mapping);
    upload.size = kUploadBufferSize;
  }

  // Sets 0-3: bindless heaps, set 4: constants.
  VkDescriptorType heap_types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
  for (int i = 0; i < 4; ++i) {
    VkDescriptorSetLayoutBinding binding = {0, heap_types[i], kHeapSize,
                                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // Textures and samplers are added while command buffers that use the heaps are recording or pending.
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
    if (dfn.vkCreateDescriptorSetLayout(vk, &info, nullptr, &heap_layouts_[i]) != VK_SUCCESS) return false;
  }
  {
    // Binding 3: the whole upload buffer of the frame, read by the vertex shaders that pull their vertices.
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
    if (dfn.vkCreateDescriptorSetLayout(vk, &info, nullptr, &constants_layout_) != VK_SUCCESS) return false;
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
    if (dfn.vkCreatePipelineLayout(vk, &info, nullptr, &draw_pipeline_layout_) != VK_SUCCESS) return false;
  }
  {
    VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeapSize * 3},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, kHeapSize},
                                     {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * kFramesInFlight},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight}};
    VkDescriptorPoolCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info.maxSets = 4 + kFramesInFlight;
    info.poolSizeCount = 4;
    info.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(vk, &info, nullptr, &draw_descriptor_pool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = draw_descriptor_pool_;
    alloc.descriptorSetCount = 4;
    alloc.pSetLayouts = heap_layouts_;
    if (dfn.vkAllocateDescriptorSets(vk, &alloc, heap_sets_) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(constants_layout_);
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts.data();
    if (dfn.vkAllocateDescriptorSets(vk, &alloc, constants_sets_.data()) != VK_SUCCESS) return false;
    for (uint32_t f = 0; f < kFramesInFlight; ++f) {
      VkDeviceSize storage_range =
          std::min<VkDeviceSize>(kUploadBufferSize, device->properties().maxStorageBufferRange);
      VkDescriptorBufferInfo buffers[4] = {{upload_[f].buffer, 0, 4096},
                                           {upload_[f].buffer, 0, 224 * 16},
                                           {upload_[f].buffer, 0, kSharedBlockSize},
                                           {upload_[f].buffer, 0, storage_range}};
      VkWriteDescriptorSet writes[4] = {};
      for (uint32_t b = 0; b < 4; ++b) {
        writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[b].dstSet = constants_sets_[f];
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = b == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[b].pBufferInfo = &buffers[b];
      }
      dfn.vkUpdateDescriptorSets(vk, 4, writes, 0, nullptr);
      pull_range_ = storage_range;
    }
  }

  // Placeholder textures (white) and sampler at index 0 of the heaps.
  auto make_dummy = [&](Image& image, VkImageType type, VkImageViewType view_type, uint32_t layers,
                        VkImageCreateFlags flags) -> bool {
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = flags;
    info.imageType = type;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                          image.image, image.memory))
      return false;
    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image.image;
    view.viewType = view_type;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    if (dfn.vkCreateImageView(vk, &view, nullptr, &image.view) != VK_SUCCESS) return false;
    image.format = info.format;
    image.width = image.height = 1;
    return true;
  };
  if (!make_dummy(dummy_2d_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0) ||
      !make_dummy(dummy_3d_, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0) ||
      !make_dummy(dummy_cube_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) {
    return false;
  }
  {
    VkSamplerCreateInfo sampler = {};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = 16.0f;
    if (dfn.vkCreateSampler(vk, &sampler, nullptr, &dummy_sampler_) != VK_SUCCESS) return false;
    VkDescriptorImageInfo images[4] = {{VK_NULL_HANDLE, dummy_2d_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, dummy_3d_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, dummy_cube_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {dummy_sampler_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = heap_sets_[i];
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = heap_types[i];
      writes[i].pImageInfo = &images[i];
    }
    dfn.vkUpdateDescriptorSets(vk, 4, writes, 0, nullptr);
  }
  InitializeTextures();
  if (!InitializeGeometry()) {
    REXGPU_WARN("[sdnx_native] geometry cache unavailable: vertex / index data copied per draw");
    ShutdownGeometry();
  }
  if (!InitializeQueries()) {
    REXGPU_WARN("[sdnx_native] occlusion queries unavailable: fixed counts (everything visible)");
    ShutdownQueries();
  }
  return true;
}

void NativeCommandProcessor::ShutdownDraws() {
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  VkDevice vk = device->device();
  ShutdownTextures();
  for (auto& it : pipelines_) dfn.vkDestroyPipeline(vk, it.second, nullptr);
  pipelines_.clear();
  for (auto* lib : {&library_vs_, &library_ps_}) {
    for (auto& it : *lib) {
      if (it.second.module) dfn.vkDestroyShaderModule(vk, it.second.module, nullptr);
    }
    lib->clear();
  }
  DestroyImage(dummy_2d_);
  DestroyImage(dummy_3d_);
  DestroyImage(dummy_cube_);
  if (dummy_sampler_) dfn.vkDestroySampler(vk, dummy_sampler_, nullptr);
  if (draw_descriptor_pool_) dfn.vkDestroyDescriptorPool(vk, draw_descriptor_pool_, nullptr);
  if (draw_pipeline_layout_) dfn.vkDestroyPipelineLayout(vk, draw_pipeline_layout_, nullptr);
  if (constants_layout_) dfn.vkDestroyDescriptorSetLayout(vk, constants_layout_, nullptr);
  for (auto& layout : heap_layouts_) {
    if (layout) dfn.vkDestroyDescriptorSetLayout(vk, layout, nullptr);
  }
  for (UploadBuffer& upload : upload_) {
    if (upload.buffer) dfn.vkDestroyBuffer(vk, upload.buffer, nullptr);
    if (upload.memory) dfn.vkFreeMemory(vk, upload.memory, nullptr);
    upload = UploadBuffer();
  }
}

VkDeviceSize NativeCommandProcessor::Upload(const void* data, VkDeviceSize size, VkDeviceSize alignment,
                                            uint8_t** mapping_out) {
  UploadBuffer& upload = upload_[uint32_t(frame_ % kFramesInFlight)];
  VkDeviceSize offset = (upload.offset + alignment - 1) & ~(alignment - 1);
  if (offset + size > upload.size) {
    ++stats_.upload_full;
    return UINT64_MAX;
  }
  if (data) std::memcpy(upload.mapping + offset, data, size_t(size));
  if (mapping_out) *mapping_out = upload.mapping + offset;
  upload.offset = offset + size;
  return offset;
}

void NativeCommandProcessor::EndRendering() {
  if (!rendering_) return;
  EndHostQuery();  // occlusion queries cannot cross rendering pass instances
  vulkan_device()->functions().vkCmdEndRendering(cb_);
  rendering_ = false;
  rendering_key_ = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The draw.

bool NativeCommandProcessor::DrawNative(xenos::PrimitiveType prim_type, uint32_t index_count,
                                        IndexBufferInfo* index_buffer_info) {
  static const bool skip_draws = std::getenv("SDNX_NO_DRAW") != nullptr;
  if (skip_draws) {
    ++stats_.skipped;
    return true;
  }
  // SDNX_PROF=1: CPU time of the parts of a draw (state/shaders, vertices, indices, constants and textures,
  // pipeline, recording), in ReportStats.
  static const bool prof = std::getenv("SDNX_PROF") != nullptr;
  uint64_t prof_t[9] = {};
  auto prof_mark = [&](int i) {
    if (prof) prof_t[i] = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
  };
  prof_mark(0);
  const RegisterFile& regs = *register_file_;
  const ui::vulkan::VulkanDevice* device = vulkan_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = device->functions();
  static const bool trace_draw = std::getenv("SDNX_TRACE_DRAW") != nullptr;
  static int trace_index = 0;
  const bool tr = trace_draw && trace_index < 200;
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: enter idx={}", trace_index, index_count);
  }
  Shader* vs = active_vertex_shader();
  Shader* ps = active_pixel_shader();
  if (log_frame_) {
    REXGPU_INFO("[sdnx_frame] enter prim={} n={} mode={} pitch={} color_mask={:#x} depthctl={:#x} depth_base={} "
                "color0_base={} vs={} ps={}",
                uint32_t(prim_type), index_count, uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode),
                uint32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch), regs[XE_GPU_REG_RB_COLOR_MASK],
                regs[XE_GPU_REG_RB_DEPTHCONTROL], uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base),
                uint32_t(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).color_base),
                vs ? 1 : 0, ps ? 1 : 0);
  }
  if (!vs || !index_count) return true;
  const VertexShaderInfo* vs_info = nullptr;
  LibraryEntry* vs_entry = FindVertexShader(vs, vs_info);
  if (!vs_entry || !GetModule(*vs_entry)) {
    ++stats_.missing_vs;
    static const bool trace_vs = std::getenv("SDNX_TRACE_VS") != nullptr;
    static int traced = 0;
    if (trace_vs && traced < 40) {
      ++traced;
      const std::vector<uint32_t>& w = vs->ucode_data();
      char head[1200];
      int off = 0;
      for (size_t k = 0; (k * 3) < w.size() && off < int(sizeof(head)) - 12; ++k) {
        off += snprintf(head + off, sizeof(head) - off, "%08X%08X%08X ", w[k * 3], w[k * 3 + 1], w[k * 3 + 2]);
      }
      REXGPU_INFO("[sdnx_vs] miss key={:016X} words={} fetches={} head={}", vs_info->masked_key, w.size(),
                  vs_info->fetches.size(), head);
    }
    return true;
  }
  if (TryRestoreDepth(ps)) return true;
  auto edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  bool color_pass = edram_mode == xenos::EdramMode::kColorDepth;
  LibraryEntry* ps_entry = nullptr;
  if (ps && color_pass) {
    ps_entry = FindPixelShader(ps);
    // SDNX_SKIP_PS=a,b,...: diagnostic, the draws of pixel shaders whose name contains one of the parts are dropped.
    static const std::vector<std::string> skip_ps = [] {
      std::vector<std::string> parts;
      if (const char* env = std::getenv("SDNX_SKIP_PS")) {
        std::stringstream ss(env);
        for (std::string part; std::getline(ss, part, ',');) {
          if (!part.empty()) parts.push_back(part);
        }
      }
      return parts;
    }();
    if (ps_entry && !skip_ps.empty()) {
      for (const std::string& part : skip_ps) {
        if (ps_entry->name.find(part) != std::string::npos) {
          ++stats_.skipped;
          return true;
        }
      }
    }
    if (!ps_entry || !GetModule(*ps_entry)) {
      ++stats_.missing_ps;
      static const bool trace_ps = std::getenv("SDNX_TRACE_PS") != nullptr;
      static int traced_ps = 0;
      if (trace_ps && traced_ps < 40) {
        ++traced_ps;
        const std::vector<uint32_t>& w = ps->ucode_data();
        char head[1200];
        int off = 0;
        for (size_t k = 0; (k * 3) < w.size() && off < int(sizeof(head)) - 12; ++k) {
          off += snprintf(head + off, sizeof(head) - off, "%08X%08X%08X ", w[k * 3], w[k * 3 + 1], w[k * 3 + 2]);
        }
        REXGPU_INFO("[sdnx_ps] miss key={:016X} words={} head={}", ps->ucode_data_hash(), w.size(), head);
      }
      return true;
    }
  }

  // Render targets.
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface.surface_pitch;
  if (!pitch) {
    ++stats_.skipped;
    if (tr) {
      REXGPU_INFO("[sdnx_trace] draw {}: skipped (surface pitch 0, mode={})", trace_index,
                  uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode));
    }
    return true;
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // Render targets are in EDRAM samples (see GetRenderTarget): MSAA draws are drawn scaled.
  uint32_t msaa_sx = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t msaa_sy = uint32_t(surface.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t need_height = std::min(scissor.offset[1] + scissor.extent[1], uint32_t(2048));
  uint32_t color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  // A target the pixel shader does not export is left alone on Xenos; in Vulkan an attachment without a fragment
  // output gets undefined values. The confetti decals of the night market road export only colour 0 with the
  // G-buffer targets 1 and 2 still enabled: the road's third target became white where the decal was transparent
  // (white, lit-up floor; 2026-11-12). Like Xenia, mask out the targets the shader does not write.
  if (ps) {
    static string::StringBuffer ps_disassembly;
    if (!ps->is_ucode_analyzed()) ps->AnalyzeUcode(ps_disassembly);
    for (uint32_t i = 0; i < 4; ++i) {
      if (!ps->writes_color_target(i)) color_mask &= ~(0xFu << (4 * i));
    }
  }
  Image* colors[4] = {};
  uint32_t color_count = 0;
  if (color_pass) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((color_mask >> (4 * i)) & 0xF)) continue;
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      colors[i] = GetRenderTarget(info.color_base, pitch, uint32_t(info.color_format), surface.msaa_samples, false,
                                  need_height);
      color_count = i + 1;
    }
  }
  auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  Image* depth = nullptr;
  if (depth_control.z_enable || depth_control.stencil_enable) {
    auto info = regs.Get<reg::RB_DEPTH_INFO>();
    depth = GetRenderTarget(info.depth_base, pitch, uint32_t(info.depth_format), surface.msaa_samples, true,
                            need_height);
  }
  if (!color_count && !depth) {
    ++stats_.skipped;
    if (tr) {
      REXGPU_INFO("[sdnx_trace] draw {}: skipped (no targets: color_mask={:#x} z={} stencil={})", trace_index,
                  color_mask, uint32_t(depth_control.z_enable), uint32_t(depth_control.stencil_enable));
    }
    return true;
  }
  uint32_t area_w = UINT32_MAX, area_h = UINT32_MAX;
  uint64_t rendering_key = 0x9E3779B97F4A7C15ull;
  if (tr) {
    uint32_t base0 = 0, fmt0 = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((color_mask >> (4 * i)) & 0xF)) continue;
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      base0 = info.color_base;
      fmt0 = uint32_t(info.color_format);
      break;
    }
    REXGPU_INFO("[sdnx_trace] draw {}: img={} base={} fmt={} pitch={} msaa={} mask={:#x}", trace_index,
                reinterpret_cast<uintptr_t>(colors[0]), base0, fmt0, pitch,
                uint32_t(surface.msaa_samples), color_mask);
  }
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: targets ready, colors={} depth={}", trace_index, color_count,
                depth != nullptr);
  }
  for (uint32_t i = 0; i < color_count; ++i) {
    if (colors[i]) {
      area_w = std::min(area_w, colors[i]->width);
      area_h = std::min(area_h, colors[i]->height);
    }
    rendering_key = rendering_key * 31 + uint64_t(reinterpret_cast<uintptr_t>(colors[i] ? colors[i]->image : 0));
  }
  if (depth) {
    area_w = std::min(area_w, depth->width);
    area_h = std::min(area_h, depth->height);
  }
  rendering_key = rendering_key * 31 + uint64_t(reinterpret_cast<uintptr_t>(depth ? depth->image : 0));

  // Viewport.
  draw_util::ViewportInfo viewport;
  draw_util::GetHostViewportInfo(regs, 1u << msaa_sx, 1u << msaa_sy, false, area_w, area_h, false,
                                 draw_util::GetNormalizedDepthControl(regs),
                                 false, false, false, viewport);
  if (!viewport.xy_extent[0] || !viewport.xy_extent[1]) {
    ++stats_.skipped;
    return true;
  }

  prof_mark(1);
  // Vertex fetches: pair the run-time fetches with the translation's K (destination register, written mask).
  struct Attribute {
    uint32_t location, binding;
    VkFormat format;
    uint32_t offset;
  };
  struct Binding {
    uint32_t slot, stride;
  };
  Attribute attributes[16];
  uint32_t attribute_count = 0;
  Binding bindings[16];
  uint32_t binding_count = 0;
  uint32_t swizzle_codes[32] = {};
  // No heap allocation per draw: malloc/free took 16 % of the GPU thread on the console.
  bool used[96] = {};
  if (vs_info->fetches.size() > 96) {
    ++stats_.skipped;
    return true;
  }
  uint32_t full_stride[96] = {};
  for (const VertexFetchRuntime& f : vs_info->fetches) {
    if (!((f.w1 >> 30) & 1)) {
      uint32_t slot = ((f.w0 >> 20) & 31) * 3 + ((f.w0 >> 25) & 3);
      full_stride[slot % 96] = f.w2 & 0xFF;
    }
  }
  int paired[32];
  std::fill(std::begin(paired), std::end(paired), -1);
  struct Repack {
    uint32_t binding, offset, format, is_signed;
  };
  Repack repacks[8];
  uint32_t repack_count = 0;
  const bool pull = vs_entry->pull;
  for (size_t k = 0; k < vs_entry->fetches.size() && k < 32; ++k) {
    const FetchSlot& slot = vs_entry->fetches[k];
    int best = -1, best_overlap = -1;
    for (size_t j = 0; j < vs_info->fetches.size(); ++j) {
      const VertexFetchRuntime& f = vs_info->fetches[j];
      if (used[j] || ((f.w0 >> 12) & 63) != slot.reg) continue;
      int overlap = __builtin_popcount(SwizzleMask(f.w1 & 0xFFF) & slot.mask);
      if (overlap > best_overlap) {
        best_overlap = overlap;
        best = int(j);
      }
    }
    if (best < 0) continue;
    used[best] = true;
    paired[k] = best;
    const VertexFetchRuntime& f = vs_info->fetches[best];
    swizzle_codes[k] = f.w1 & 0xFFF;
    if (pull || slot.location == 0xFF || attribute_count >= 16) continue;
    uint32_t fetch_slot = (((f.w0 >> 20) & 31) * 3 + ((f.w0 >> 25) & 3)) % 96;
    uint32_t stride = (f.w2 & 0xFF) ? (f.w2 & 0xFF) : full_stride[fetch_slot];
    int32_t offset = int32_t((f.w2 >> 8) & 0x7FFFFF) << 9 >> 9;
    VkFormat format = VertexFormat((f.w1 >> 16) & 63, (f.w1 >> 12) & 1, !((f.w1 >> 13) & 1));
    // SDNX_TRACE_FETCH=<part of a vertex shader name>: the vertex fetches of that shader, once per K.
    static const char* trace_fetch = std::getenv("SDNX_TRACE_FETCH");
    if (trace_fetch && vs_entry->name.find(trace_fetch) != std::string::npos) {
      static std::unordered_map<uint64_t, int> traced_fetches;
      uint64_t tkey = uint64_t(k) | uint64_t(f.w1) << 8 | uint64_t(f.w2) << 40;
      if (traced_fetches[tkey]++ == 0) {
        xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(fetch_slot);
        REXGPU_INFO("[sdnx_fetch] {} K{} loc {} reg {} slot {} offset {} stride {} format {} signed {} norm {} "
                    "exp_adjust {} swizzle {:#x} mini {} -> vk {} (buffer {:#x} size {} endian {})",
                    vs_entry->name.substr(vs_entry->name.find_last_of("/\\") + 1), k, uint32_t(slot.location),
                    uint32_t(slot.reg), fetch_slot, offset, stride, (f.w1 >> 16) & 63, (f.w1 >> 12) & 1,
                    !((f.w1 >> 13) & 1), int32_t(f.w1 << 2) >> 26, f.w1 & 0xFFF, (f.w1 >> 30) & 1, int(format),
                    uint32_t(vf.address) << 2, uint32_t(vf.size) << 2, uint32_t(vf.endian));
      }
    }
    // k_10_11_11 / k_11_11_10 (packed normals) have no Vulkan vertex format: the copy of the buffer gets that dword
    // repacked to 8:8:8:8 (see RepackNormals) and the attribute reads it as such. Without it the night market road
    // had no normal at all (white floor, 2026-11-12).
    uint32_t vertex_format = (f.w1 >> 16) & 63;
    bool repack = false;
    if (format == VK_FORMAT_UNDEFINED && (vertex_format == 16 || vertex_format == 17)) {
      bool is_signed = (f.w1 >> 12) & 1;
      format = is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
      repack = true;
    }
    if (format == VK_FORMAT_UNDEFINED) continue;
    uint32_t b = 0;
    for (; b < binding_count; ++b) {
      if (bindings[b].slot == fetch_slot) break;
    }
    if (b == binding_count) {
      if (binding_count >= 16) continue;
      bindings[binding_count++] = {fetch_slot, stride * 4};
    }
    if (repack && repack_count < 8) {
      repacks[repack_count++] = {b, uint32_t(std::max(offset, 0)) * 4, vertex_format, uint32_t((f.w1 >> 12) & 1)};
    }
    bool dup = false;
    for (uint32_t a = 0; a < attribute_count; ++a) dup |= attributes[a].location == slot.location;
    if (!dup) attributes[attribute_count++] = {slot.location, b, format, uint32_t(std::max(offset, 0)) * 4};
  }

  prof_mark(7);
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: ensure frame", trace_index);
  }
  EnsureFrame();
  UploadBuffer& upload = upload_[uint32_t(frame_ % kFramesInFlight)];

  // Pulling vertex shaders: every fetch slot they use is copied whole (in host word order, like a vertex buffer)
  // into the upload buffer; fetch K gets {byte offset of its first element, stride in bytes, VkFormat, vertex count}
  // at kPullInfoOffset + K * 16 of the shared block. No vertex attributes are bound.
  uint32_t pull_info[16][4] = {};
  if (pull) {
    uint32_t copied_slot[16], copied_offset[16], copied_count = 0;
    for (size_t k = 0; k < vs_entry->fetches.size() && k < 16; ++k) {
      if (paired[k] < 0) continue;
      const VertexFetchRuntime& f = vs_info->fetches[paired[k]];
      uint32_t fetch_slot = (((f.w0 >> 20) & 31) * 3 + ((f.w0 >> 25) & 3)) % 96;
      uint32_t stride = (f.w2 & 0xFF) ? (f.w2 & 0xFF) : full_stride[fetch_slot];
      int32_t offset = int32_t((f.w2 >> 8) & 0x7FFFFF) << 9 >> 9;
      VkFormat format = VertexFormat((f.w1 >> 16) & 63, (f.w1 >> 12) & 1, !((f.w1 >> 13) & 1));
      if (format == VK_FORMAT_UNDEFINED || !stride) continue;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(fetch_slot);
      uint32_t address = uint32_t(fetch.address) << 2, size = uint32_t(fetch.size) << 2;
      uint32_t c = 0;
      for (; c < copied_count; ++c) {
        if (copied_slot[c] == fetch_slot) break;
      }
      if (c == copied_count) {
        const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(address);
        uint8_t* dst = nullptr;
        VkDeviceSize at = (src && size) ? Upload(nullptr, size, 16, &dst) : UINT64_MAX;
        if (at == UINT64_MAX || at + size > pull_range_) {
          ++stats_.skipped;
          return true;
        }
        CopySwapped(dst, src, size, fetch.endian);
        stats_.geo_direct_bytes += size;
        copied_slot[copied_count] = fetch_slot;
        copied_offset[copied_count++] = uint32_t(at);
      }
      pull_info[k][0] = copied_offset[c] + uint32_t(std::max(offset, 0)) * 4;
      pull_info[k][1] = stride * 4;
      // exp_adjust (6-bit signed, fetch word 1 bits 24-29) in bits 16-21.
      pull_info[k][2] = uint32_t(format) | (((f.w1 >> 24) & 63) << 16);
      pull_info[k][3] = size / (stride * 4);
    }
    ++stats_.pulled;
  }
  prof_mark(8);

  // Vertex data. Non-indexed rectangle lists get the 4th vertex of every rectangle appended (after the data of
  // every binding, at the same index): it is extrapolated in input space, which is exact for affine vertex
  // shaders - the clears and screen quads that use rectangle lists.
  bool rects = prim_type == xenos::PrimitiveType::kRectangleList &&
               !(index_buffer_info && index_buffer_info->guest_base);
  uint32_t rect_count = rects ? index_count / 3 : 0;
  uint32_t extra_first = 0;
  uint32_t vb_size[16] = {};
  for (uint32_t b = 0; b < binding_count; ++b) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(bindings[b].slot);
    vb_size[b] = uint32_t(fetch.size) << 2;
    if (rect_count && bindings[b].stride) {
      extra_first = std::max(extra_first, (vb_size[b] + bindings[b].stride - 1) / bindings[b].stride);
    }
  }
  VkBuffer vb_buffers[16];
  VkDeviceSize vb_offsets[16];
  uint8_t* vb_data[16] = {};
  // Vertex buffers that are not cached are copied only in the range of vertices the draw uses (found from the
  // guest indices, once, when the first binding misses the cache): whole buffers were 22-50 MB per frame.
  // The draw then uses vertexOffset = -range_first, and cached bindings are bound range_first vertices further.
  bool range_known = false, vb_cached[16] = {};
  uint32_t range_first = 0, range_last = 0;
  // Indices through the content-hashed copies (once per draw, on first need): the converted 32-bit indices and
  // their min / max, so neither the conversion nor the scan for the vertex range is redone when they did not change.
  const bool draw_restart =
      regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
      (prim_type == xenos::PrimitiveType::kTriangleStrip || prim_type == xenos::PrimitiveType::kLineStrip ||
       prim_type == xenos::PrimitiveType::kTriangleFan);
  const bool draw_quads = prim_type == xenos::PrimitiveType::kQuadList;
  HashedGeometry* index_hashed = nullptr;
  bool index_hash_tried = false;
  auto index_hashed_lookup = [&]() -> HashedGeometry* {
    if (index_hash_tried) return index_hashed;
    index_hash_tried = true;
    if (!(index_buffer_info && index_buffer_info->guest_base) || rect_count || !index_count) return nullptr;
    const uint8_t* isrc = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
    if (!isrc) return nullptr;
    const bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    const xenos::Endian ie = index_buffer_info->endianness;
    const bool swap16 = ie == xenos::Endian::k8in16 || ie == xenos::Endian::k8in32;
    const uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    const uint32_t guest_size = index_count * (is32 ? 4 : 2);
    const uint32_t host_count = draw_quads ? index_count / 4 * 6 : index_count;
    if (!host_count) return nullptr;
    uint64_t key_data[3] = {uint64_t(index_buffer_info->guest_base) | uint64_t(index_count) << 32,
                            uint64_t(is32) | uint64_t(ie) << 1 | uint64_t(draw_restart) << 4 |
                                uint64_t(draw_quads) << 5 | uint64_t(reset) << 8,
                            0x48494E444558ull};
    const uint64_t ikey = XXH3_64bits(key_data, sizeof(key_data));
    if ((index_hashed = HashedReuse(ikey, VkDeviceSize(host_count) * 4))) return index_hashed;
    const uint64_t content = XXH3_64bits(isrc, guest_size);
    stats_.hash_bytes += guest_size;
    uint32_t lo = UINT32_MAX, hi = 0;
    bool filled = false;
    index_hashed = HashedCopy(ikey, content, VkDeviceSize(host_count) * 4,
                              [&](uint8_t* out) {
                                filled = true;
                                auto read = [&](uint32_t i) -> uint32_t {
                                  uint32_t v;
                                  if (is32) {
                                    std::memcpy(&v, isrc + i * 4, 4);
                                    v = ie == xenos::Endian::k8in32 ? ByteSwap32(v) : v;
                                  } else {
                                    uint16_t h;
                                    std::memcpy(&h, isrc + i * 2, 2);
                                    v = swap16 ? uint16_t((h >> 8) | (h << 8)) : h;
                                  }
                                  if (draw_restart && v == reset) return 0xFFFFFFFFu;
                                  lo = std::min(lo, v);
                                  hi = std::max(hi, v);
                                  return v;
                                };
                                uint32_t* o = reinterpret_cast<uint32_t*>(out);
                                if (draw_quads) {
                                  for (uint32_t q = 0; q + 3 < index_count; q += 4) {
                                    uint32_t a = read(q), b = read(q + 1), c = read(q + 2), d = read(q + 3);
                                    *o++ = a; *o++ = b; *o++ = c; *o++ = a; *o++ = c; *o++ = d;
                                  }
                                } else {
                                  for (uint32_t i = 0; i < index_count; ++i) o[i] = read(i);
                                }
                              });
    if (index_hashed && filled) {
      index_hashed->index_min = lo <= hi ? lo : 0;
      index_hashed->index_max = lo <= hi ? hi : 0;
    }
    return index_hashed;
  };
  auto find_range = [&]() {
    range_known = true;
    range_first = 0;
    range_last = index_count ? index_count - 1 : 0;
    if (!(index_buffer_info && index_buffer_info->guest_base)) return;
    if (HashedGeometry* hashed = index_hashed_lookup()) {
      range_first = hashed->index_min;
      range_last = hashed->index_max;
      return;
    }
    const uint8_t* isrc = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
    if (!isrc) return;
    bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    xenos::Endian ie = index_buffer_info->endianness;
    bool use_reset = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena;
    uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < index_count; ++i) {
      uint32_t v;
      if (is32) {
        std::memcpy(&v, isrc + i * 4, 4);
        v = ie == xenos::Endian::k8in32 ? ByteSwap32(v) : v;
      } else {
        uint16_t h;
        std::memcpy(&h, isrc + i * 2, 2);
        v = (ie == xenos::Endian::k8in16 || ie == xenos::Endian::k8in32) ? uint16_t((h >> 8) | (h << 8)) : h;
      }
      if (use_reset && v == reset) continue;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    if (lo <= hi) {
      range_first = lo;
      range_last = hi;
    }
  };
  for (uint32_t b = 0; b < binding_count; ++b) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(bindings[b].slot);
    uint32_t address = uint32_t(fetch.address) << 2;
    uint32_t size = vb_size[b];
    uint32_t alloc = rect_count ? std::max(size, (extra_first + rect_count) * bindings[b].stride) : size;
    const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(address);
    // Packed normals of this binding repacked in the copy (part of the cache key: the raw copy is another entry).
    uint64_t repack_key = 0;
    for (uint32_t r = 0; r < repack_count; ++r) {
      if (repacks[r].binding == b) {
        repack_key = repack_key * 31 + (uint64_t(repacks[r].offset) << 8 | repacks[r].format << 1 | repacks[r].is_signed) + 1;
      }
    }
    // The upload and geometry buffers are write-combined mappings: reading them back to repack was ~3x slower
    // than the whole frame (22 fps). With normals to repack, the copy goes through ordinary memory first.
    auto copy_vertices = [&](uint8_t* out, const uint8_t* from, uint32_t count, xenos::Endian endian) {
      if (!repack_key) {
        CopySwapped(out, from, count, endian);
        return;
      }
      static thread_local std::vector<uint8_t> scratch;
      scratch.resize(count);
      CopySwapped(scratch.data(), from, count, endian);
      for (uint32_t r = 0; r < repack_count; ++r) {
        if (repacks[r].binding != b) continue;
        RepackNormals(scratch.data(), count, bindings[b].stride, repacks[r].offset, repacks[r].format,
                      repacks[r].is_signed != 0);
      }
      std::memcpy(out, scratch.data(), count);
    };
    if (!rect_count && src && size) {
      uint64_t key_data[3] = {uint64_t(address) | uint64_t(size) << 32, uint64_t(fetch.endian) | 0x56455254580000ull,
                              repack_key};
      uint64_t key = XXH3_64bits(key_data, repack_key ? sizeof(key_data) : sizeof(uint64_t) * 2);
      xenos::Endian endian = fetch.endian;
      VkDeviceSize cached = CachedGeometry(key, address, size, size, [&](uint8_t* out) {
        copy_vertices(out, src, size, endian);
      });
      if (cached != UINT64_MAX) {
        vb_buffers[b] = geometry_buffer_;
        vb_offsets[b] = cached;
        vb_cached[b] = true;
        continue;
      }
      uint32_t stride = bindings[b].stride;
      if (stride) {
        if (!range_known) find_range();
        uint64_t begin = uint64_t(range_first) * stride;
        uint64_t end = std::min<uint64_t>(size, (uint64_t(range_last) + 2) * stride);  // + 1 vertex of slack
        if (begin < end) {
          uint32_t part = uint32_t(end - begin);
          // The used range through the content-hashed copies: hashed (read only), copied only when it changed.
          {
            uint64_t vkey_data[4] = {uint64_t(address) | uint64_t(size) << 32,
                                     uint64_t(endian) | uint64_t(stride) << 8 | 0x48565254580000ull, repack_key,
                                     begin | uint64_t(part) << 32};
            const uint64_t vkey = XXH3_64bits(vkey_data, sizeof(vkey_data));
            HashedGeometry* hashed = HashedReuse(vkey, part);
            if (!hashed) {
              uint64_t content = XXH3_64bits(src + begin, part);
              stats_.hash_bytes += part;
              stats_.hash_whole_bytes += size;
              hashed = HashedCopy(vkey, content, part,
                                  [&](uint8_t* out) { copy_vertices(out, src + begin, part, endian); });
            }
            if (hashed) {
              vb_buffers[b] = hashed_buffer_;
              vb_offsets[b] = hashed->offset;
              continue;
            }
          }
          uint8_t* part_dst = nullptr;
          VkDeviceSize part_offset = Upload(nullptr, part, 16, &part_dst);
          if (part_offset == UINT64_MAX) {
            ++stats_.skipped;
            return true;
          }
          copy_vertices(part_dst, src + begin, part, endian);
          stats_.geo_direct_bytes += part;
          vb_buffers[b] = upload.buffer;
          vb_offsets[b] = part_offset;
          continue;
        }
      }
      stats_.geo_direct_bytes += size;
    }
    uint8_t* dst = nullptr;
    VkDeviceSize offset = size ? Upload(nullptr, alloc, 16, &dst) : UINT64_MAX;
    if (!src || offset == UINT64_MAX) {
      ++stats_.skipped;
      return true;
    }
    copy_vertices(dst, src, size, fetch.endian);
    vb_buffers[b] = upload.buffer;
    vb_offsets[b] = offset;
    vb_data[b] = dst;
    vb_cached[b] = true;  // whole buffer from vertex 0, like a cached one
  }
  int32_t vertex_offset = 0;
  if (range_known && range_first) {
    vertex_offset = -int32_t(range_first);
    for (uint32_t b = 0; b < binding_count; ++b) {
      if (vb_cached[b]) vb_offsets[b] += VkDeviceSize(range_first) * bindings[b].stride;
    }
  }
  std::vector<uint32_t> rect_indices;
  // Bounds of the rectangles (guest window coordinates) and the z of the first one, for the EDRAM alias below.
  float rect_bounds[4] = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
  float rect_z = 0.0f;
  bool rect_has_z = false;
  if (rect_count) {
    auto float_count = [](VkFormat f) -> uint32_t {
      switch (f) {
        case VK_FORMAT_R32_SFLOAT: return 1;
        case VK_FORMAT_R32G32_SFLOAT: return 2;
        case VK_FORMAT_R32G32B32_SFLOAT: return 3;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 4;
        default: return 0;
      }
    };
    const Attribute* position = nullptr;
    for (uint32_t a = 0; a < attribute_count; ++a) {
      if (attributes[a].location == 0 && float_count(attributes[a].format) >= 2) position = &attributes[a];
    }
    auto vertex = [&](uint32_t b, uint32_t index) { return vb_data[b] + size_t(index) * bindings[b].stride; };
    rect_indices.reserve(rect_count * 6);
    for (uint32_t r = 0; r < rect_count; ++r) {
      uint32_t v[3] = {r * 3, r * 3 + 1, r * 3 + 2};
      // The diagonal is the longest edge; the missing vertex mirrors the corner across it.
      uint32_t corner = 0;
      if (position) {
        float p[3][2];
        for (int i = 0; i < 3; ++i) {
          std::memcpy(p[i], vertex(position->binding, v[i]) + position->offset, 8);
        }
        auto d2 = [&](int i, int j) {
          return (p[i][0] - p[j][0]) * (p[i][0] - p[j][0]) + (p[i][1] - p[j][1]) * (p[i][1] - p[j][1]);
        };
        for (int i = 0; i < 3; ++i) {
          rect_bounds[0] = std::min(rect_bounds[0], p[i][0]);
          rect_bounds[1] = std::min(rect_bounds[1], p[i][1]);
          rect_bounds[2] = std::max(rect_bounds[2], p[i][0]);
          rect_bounds[3] = std::max(rect_bounds[3], p[i][1]);
        }
        if (r == 0 && float_count(position->format) >= 3) {
          std::memcpy(&rect_z, vertex(position->binding, v[0]) + position->offset + 8, 4);
          rect_has_z = true;
        }
        float d01 = d2(0, 1), d02 = d2(0, 2), d12 = d2(1, 2);
        corner = (d12 >= d01 && d12 >= d02) ? 0 : (d02 >= d01 ? 1 : 2);
        if (log_frame_) {
          REXGPU_INFO("[sdnx_frame] rect {}: ({}, {}) ({}, {}) ({}, {}) corner {}", r, p[0][0], p[0][1], p[1][0],
                      p[1][1], p[2][0], p[2][1], corner);
        }
      }
      uint32_t c = v[corner], e0 = v[(corner + 1) % 3], e1 = v[(corner + 2) % 3];
      uint32_t added = extra_first + r;
      for (uint32_t b = 0; b < binding_count; ++b) {
        if (bindings[b].stride) std::memcpy(vertex(b, added), vertex(b, c), bindings[b].stride);
      }
      for (uint32_t a = 0; a < attribute_count; ++a) {
        uint32_t n = float_count(attributes[a].format);
        uint32_t b = attributes[a].binding;
        if (!n || !bindings[b].stride) continue;
        float fc[4], f0[4], f1[4], out[4];
        std::memcpy(fc, vertex(b, c) + attributes[a].offset, n * 4);
        std::memcpy(f0, vertex(b, e0) + attributes[a].offset, n * 4);
        std::memcpy(f1, vertex(b, e1) + attributes[a].offset, n * 4);
        for (uint32_t i = 0; i < n; ++i) out[i] = f0[i] + f1[i] - fc[i];
        std::memcpy(vertex(b, added) + attributes[a].offset, out, n * 4);
      }
      rect_indices.insert(rect_indices.end(), {v[0], v[1], v[2], e0, e1, added});
    }
  }

  prof_mark(2);
  // Indices (and primitive conversions).
  bool restart = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
                 (prim_type == xenos::PrimitiveType::kTriangleStrip || prim_type == xenos::PrimitiveType::kLineStrip ||
                  prim_type == xenos::PrimitiveType::kTriangleFan);
  VkPrimitiveTopology topology;
  switch (prim_type) {
    case xenos::PrimitiveType::kPointList: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case xenos::PrimitiveType::kLineList: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kLineLoop: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case xenos::PrimitiveType::kTriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case xenos::PrimitiveType::kTriangleFan: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    default: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;  // lists, rectangles (approximate), quads
  }
  bool quads = prim_type == xenos::PrimitiveType::kQuadList;
  {
    static const bool trace_prim = std::getenv("SDNX_TRACE_PRIM") != nullptr;
    static uint32_t seen_prims = 0;
    if (trace_prim && !((seen_prims >> uint32_t(prim_type)) & 1)) {
      seen_prims |= 1u << uint32_t(prim_type);
      REXGPU_INFO("[sdnx_prim] first draw with primitive type {} ({} indices, indexed {})", uint32_t(prim_type),
                  index_count, index_buffer_info && index_buffer_info->guest_base ? 1 : 0);
    }
  }
  std::vector<uint32_t> indices;
  bool indexed = index_buffer_info && index_buffer_info->guest_base;
  VkBuffer index_buffer = upload.buffer;
  VkDeviceSize index_offset = 0;
  uint32_t index_draw_count = 0;
  bool index_cached = false;
  // Already found through the content-hashed copies (for the vertex range): use that copy.
  if (indexed && !rect_count && index_hash_tried && index_hashed) {
    index_buffer = hashed_buffer_;
    index_offset = index_hashed->offset;
    index_draw_count = quads ? index_count / 4 * 6 : index_count;
    index_cached = true;
  }
  if (indexed && !rect_count && !index_cached) {
    // Converted indices (32-bit, restart as 0xFFFFFFFF, quads as triangles) of static index buffers are cached.
    bool is32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    uint32_t guest_size = index_count * (is32 ? 4 : 2);
    uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    uint32_t host_count = quads ? index_count / 4 * 6 : index_count;
    uint64_t key_data[3] = {uint64_t(index_buffer_info->guest_base) | uint64_t(index_count) << 32,
                            uint64_t(is32) | uint64_t(index_buffer_info->endianness) << 1 | uint64_t(restart) << 4 |
                                uint64_t(quads) << 5 | uint64_t(reset) << 8,
                            0x494E444558ull};
    const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
    if (src && host_count) {
      xenos::Endian endian = index_buffer_info->endianness;
      VkDeviceSize cached = CachedGeometry(
          XXH3_64bits(key_data, sizeof(key_data)), index_buffer_info->guest_base, guest_size,
          VkDeviceSize(host_count) * 4, [&](uint8_t* out) {
            uint32_t* o = reinterpret_cast<uint32_t*>(out);
            auto read = [&](uint32_t i) -> uint32_t {
              uint32_t v;
              if (is32) {
                std::memcpy(&v, src + i * 4, 4);
                v = endian == xenos::Endian::k8in32 ? ByteSwap32(v) : v;
              } else {
                uint16_t h;
                std::memcpy(&h, src + i * 2, 2);
                v = (endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32) ? uint16_t((h >> 8) | (h << 8))
                                                                                          : h;
              }
              return (restart && v == reset) ? 0xFFFFFFFFu : v;
            };
            if (quads) {
              for (uint32_t q = 0; q + 3 < index_count; q += 4) {
                uint32_t a = read(q), b = read(q + 1), c = read(q + 2), d = read(q + 3);
                *o++ = a; *o++ = b; *o++ = c; *o++ = a; *o++ = c; *o++ = d;
              }
            } else {
              for (uint32_t i = 0; i < index_count; ++i) o[i] = read(i);
            }
          });
      if (cached != UINT64_MAX) {
        index_buffer = geometry_buffer_;
        index_offset = cached;
        index_draw_count = host_count;
        index_cached = true;
      } else if (HashedGeometry* hashed = index_hashed_lookup()) {
        index_buffer = hashed_buffer_;
        index_offset = hashed->offset;
        index_draw_count = host_count;
        index_cached = true;
      } else {
        stats_.geo_direct_bytes += guest_size;
      }
    }
  }
  if (index_cached) {
    // indices stays empty: the cached copy is used.
  } else if (rect_count) {
    indices.swap(rect_indices);
  } else if (indexed || quads) {
    // Converted straight into the upload buffer (a vector with push_back per index was malloc/free and copies;
    // ~1.5 million indices a frame in gameplay).
    const uint8_t* src = nullptr;
    if (indexed) {
      src = memory_->TranslatePhysical<const uint8_t*>(index_buffer_info->guest_base);
      if (!src) {
        ++stats_.skipped;
        return true;
      }
    }
    const bool is32 = indexed && index_buffer_info->format == xenos::IndexFormat::kInt32;
    const xenos::Endian endian = indexed ? index_buffer_info->endianness : xenos::Endian::kNone;
    const bool swap16 = endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32;
    const uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
    auto read = [&](uint32_t i) -> uint32_t {
      if (!indexed) return i;
      uint32_t v;
      if (is32) {
        std::memcpy(&v, src + i * 4, 4);
        v = endian == xenos::Endian::k8in32 ? ByteSwap32(v) : v;
      } else {
        uint16_t h;
        std::memcpy(&h, src + i * 2, 2);
        v = swap16 ? uint16_t((h >> 8) | (h << 8)) : h;
      }
      return (restart && v == reset) ? 0xFFFFFFFFu : v;
    };
    const uint32_t host_count = quads ? index_count / 4 * 6 : index_count;
    uint8_t* dst = nullptr;
    index_offset = host_count ? Upload(nullptr, VkDeviceSize(host_count) * 4, 4, &dst) : UINT64_MAX;
    if (index_offset == UINT64_MAX) {
      ++stats_.skipped;
      return true;
    }
    uint32_t* o = reinterpret_cast<uint32_t*>(dst);
    if (quads) {
      for (uint32_t q = 0; q + 3 < index_count; q += 4) {
        uint32_t a = read(q), b = read(q + 1), c = read(q + 2), d = read(q + 3);
        *o++ = a; *o++ = b; *o++ = c; *o++ = a; *o++ = c; *o++ = d;
      }
    } else {
      for (uint32_t i = 0; i < index_count; ++i) o[i] = read(i);
    }
    index_draw_count = host_count;
  }
  if (!indices.empty()) {
    index_offset = Upload(indices.data(), indices.size() * 4, 4);
    if (index_offset == UINT64_MAX) {
      ++stats_.skipped;
      return true;
    }
    index_draw_count = uint32_t(indices.size());
  }

  prof_mark(3);
  // Constants: VS c0-c255, PS c256-c479, shared block.
  VkDeviceSize ubo_align = std::max<VkDeviceSize>(device->properties().minUniformBufferOffsetAlignment, 16);
  // The bound ranges stay 4096 / 3584 bytes, but only the registers up to the last one the shader reads are copied
  // (whole 8.6 KB per draw was ~40 MB of memcpy a frame on the console). Dynamic addressing: everything.
  auto used_constant_bytes = [](Shader* shader, uint32_t registers) -> uint32_t {
    if (!shader) return 0;
    static string::StringBuffer constants_disassembly;
    if (!shader->is_ucode_analyzed()) shader->AnalyzeUcode(constants_disassembly);
    const Shader::ConstantRegisterMap& map = shader->constant_register_map();
    if (map.float_dynamic_addressing) return registers * 16;
    for (int w = int(registers / 64) - 1 + int(registers % 64 != 0); w >= 0; --w) {
      uint64_t bits = map.float_bitmap[w];
      if (w == int(registers / 64) && registers % 64) bits &= (uint64_t(1) << (registers % 64)) - 1;
      if (bits) return std::min(registers, uint32_t(w * 64 + 64 - __builtin_clzll(bits))) * 16;
    }
    return 0;
  };
  // The upload buffer is write-combined memory: on the console every byte written there per draw was expensive (the
  // store after the copies - the pipeline key's zero fill - held 11 % of the GPU thread, 2026-11-13). A block equal to
  // the one uploaded last in this frame reuses that copy: compared in ordinary memory, nothing written.
  struct UploadedCopy {
    uint64_t frame = UINT64_MAX;
    VkDeviceSize offset = 0;
    uint32_t range = 0, bytes = 0;
    alignas(16) uint8_t data[4096];
  };
  static UploadedCopy vs_copy, ps_copy, shared_copy;
  auto upload_reused = [&](UploadedCopy& copy, const void* values, uint32_t range, uint32_t used_bytes) -> VkDeviceSize {
    used_bytes = std::min(used_bytes, range);
    if (copy.frame == frame_ && copy.range == range && copy.bytes == used_bytes &&
        std::memcmp(copy.data, values, used_bytes) == 0) {
      ++stats_.constants_reused;
      return copy.offset;
    }
    uint8_t* dst = nullptr;
    VkDeviceSize at = Upload(nullptr, range, ubo_align, &dst);
    if (at == UINT64_MAX) return at;
    if (used_bytes) {
      std::memcpy(dst, values, used_bytes);
      std::memcpy(copy.data, values, used_bytes);
    }
    copy.frame = frame_;
    copy.offset = at;
    copy.range = range;
    copy.bytes = used_bytes;
    return at;
  };
  VkDeviceSize vs_constants = upload_reused(vs_copy, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], 4096,
                                            used_constant_bytes(vs, 256));
  VkDeviceSize ps_constants = upload_reused(ps_copy, &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X], 224 * 16,
                                            ps_entry ? used_constant_bytes(ps, 224) : 0);
  uint32_t shared[kSharedBlockSize / 4] = {};
  shared[256 / 4] = regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031];
  float alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  std::memcpy(&shared[272 / 4], &alpha_ref, 4);
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  shared[276 / 4] = uint32_t(color_control.alpha_func);
  // The library is compiled with -fvk-invert-y (the shader negates Y for Vulkan) and GetHostViewportInfo with
  // a top-left origin flips Y as well: undo one of the two, otherwise the picture is upside down.
  float ndc_scale[2] = {viewport.ndc_scale[0], -viewport.ndc_scale[1]};
  float ndc_offset[2] = {viewport.ndc_offset[0], -viewport.ndc_offset[1]};
  std::memcpy(&shared[280 / 4], ndc_scale, 8);
  std::memcpy(&shared[288 / 4], ndc_offset, 8);
  float one = 1.0f;
  for (uint32_t s = 0; s < 32; ++s) {
    std::memcpy(&shared[(360 + s * 8) / 4], &one, 4);
    std::memcpy(&shared[(364 + s * 8) / 4], &one, 4);
  }
  for (uint32_t k = 0; k < 32; ++k) shared[(624 / 4) + k / 2] |= swizzle_codes[k] << ((k & 1) * 16);
  if (pull) std::memcpy(&shared[kPullInfoOffset / 4], pull_info, sizeof(pull_info));
  BindTextures(vs, ps_entry ? ps : nullptr, shared);
  VkDeviceSize shared_constants = upload_reused(shared_copy, shared, sizeof(shared), sizeof(shared));
  if (vs_constants == UINT64_MAX || ps_constants == UINT64_MAX || shared_constants == UINT64_MAX) {
    ++stats_.skipped;
    return true;
  }

  prof_mark(4);
  // Pipeline.
  uint32_t spec = kSpecConstantsUbo;
  if (color_control.alpha_test_enable && ps_entry) {
    spec |= kSpecAlphaTest | (uint32_t(color_control.alpha_func) << 16);
  }
  struct PipelineKey {
    uint64_t vs, ps;
    uint32_t topology, restart, spec, attribute_count, binding_count;
    uint32_t attr[16][4];
    uint32_t bind[16];
    uint32_t color_formats[4], depth_format, color_count;
    uint32_t blend[4], color_mask;
    uint32_t depth_test, depth_write, depth_func;
    uint32_t stencil;  // RB_DEPTHCONTROL stencil bits (0 when disabled)
    uint32_t cull;     // bit 0 front, bit 1 back, bit 2 front face clockwise
  } key = {};
  key.vs = reinterpret_cast<uintptr_t>(vs_entry);
  key.ps = reinterpret_cast<uintptr_t>(ps_entry);
  key.topology = topology;
  key.restart = restart;
  key.spec = spec;
  key.attribute_count = attribute_count;
  key.binding_count = binding_count;
  for (uint32_t a = 0; a < attribute_count; ++a) {
    key.attr[a][0] = attributes[a].location;
    key.attr[a][1] = attributes[a].binding;
    key.attr[a][2] = attributes[a].format;
    key.attr[a][3] = attributes[a].offset;
  }
  for (uint32_t b = 0; b < binding_count; ++b) key.bind[b] = bindings[b].stride;
  key.color_count = color_count;
  for (uint32_t i = 0; i < color_count; ++i) {
    key.color_formats[i] = colors[i] ? colors[i]->format : VK_FORMAT_UNDEFINED;
    key.blend[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]];
  }
  key.color_mask = color_mask;
  key.depth_format = depth ? depth->format : VK_FORMAT_UNDEFINED;
  key.depth_test = depth && depth_control.z_enable;
  key.depth_write = depth && depth_control.z_write_enable;
  key.depth_func = uint32_t(depth_control.zfunc);
  {
    // As the SDK Vulkan backend: the host mapping is GetHostViewportInfo's (see the NDC note above), where the
    // guest face bit means clockwise. Rectangle lists are expanded here, their winding is not the guest's.
    auto mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    static const bool no_cull = std::getenv("SDNX_NO_CULL") != nullptr;
    if (!no_cull && draw_util::IsPrimitivePolygonal(regs) && prim_type != xenos::PrimitiveType::kRectangleList) {
      key.cull = uint32_t(mode_cntl.cull_front) | uint32_t(mode_cntl.cull_back) << 1 |
                 uint32_t(mode_cntl.face != 0) << 2;
    }
  }
  static const bool no_stencil = std::getenv("SDNX_NO_STENCIL") != nullptr;
  // SDNX_NO_STENCIL_PS=<part of a pixel shader name>: no stencil test for the draws of that shader only.
  static const char* no_stencil_ps = std::getenv("SDNX_NO_STENCIL_PS");
  const bool skip_stencil =
      no_stencil || (no_stencil_ps && ps_entry && ps_entry->name.find(no_stencil_ps) != std::string::npos);
  if (depth && depth_control.stencil_enable && !skip_stencil) {
    // Bits 0 and 7-31: enable, backface enable, front and back functions and operations.
    key.stencil = depth_control.value & 0xFFFFFF81u;
    if (!depth_control.backface_enable) key.stencil &= 0x0001FFFFu;
  }
  uint64_t pipeline_hash = XXH3_64bits(&key, sizeof(key));
  VkPipeline& pipeline = pipelines_[pipeline_hash];
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: shaders ok, attrs={} binds={} colors={} depth={} pipeline={:#x}", trace_index,
                attribute_count, binding_count, color_count, depth != nullptr, pipeline_hash);
  }
  if (!pipeline) {
    if (tr) {
      REXGPU_INFO("[sdnx_trace] draw {}: creating pipeline vs={} ps={} spec={:#x}", trace_index, vs_entry->name,
                  ps_entry ? ps_entry->name : "-", spec);
    }
    ++stats_.new_pipelines;
    VkSpecializationMapEntry spec_entry = {0, 0, 4};
    VkSpecializationInfo spec_info = {1, &spec_entry, 4, &spec};
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_entry->module;
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = &spec_info;
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_entry ? ps_entry->module : VK_NULL_HANDLE;
    VkVertexInputAttributeDescription vk_attributes[16];
    VkVertexInputBindingDescription vk_bindings[16];
    for (uint32_t a = 0; a < attribute_count; ++a) {
      vk_attributes[a] = {attributes[a].location, attributes[a].binding, attributes[a].format, attributes[a].offset};
    }
    for (uint32_t b = 0; b < binding_count; ++b) {
      vk_bindings[b] = {b, bindings[b].stride, VK_VERTEX_INPUT_RATE_VERTEX};
    }
    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = binding_count;
    vertex_input.pVertexBindingDescriptions = vk_bindings;
    vertex_input.vertexAttributeDescriptionCount = attribute_count;
    vertex_input.pVertexAttributeDescriptions = vk_attributes;
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = topology;
    input_assembly.primitiveRestartEnable = restart;
    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = ((key.cull & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((key.cull & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    raster.frontFace = (key.cull & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = depth ? VK_TRUE : VK_FALSE;  // the factors are dynamic (zero for most draws)
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = key.depth_test;
    depth_stencil.depthWriteEnable = key.depth_write;
    depth_stencil.depthCompareOp = VkCompareOp(key.depth_func);
    if (key.stencil) {
      // Xenos CompareFunction and StencilOp have the Vulkan values. Reference and masks are dynamic.
      reg::RB_DEPTHCONTROL dc;
      dc.value = key.stencil;
      depth_stencil.stencilTestEnable = VK_TRUE;
      depth_stencil.front.compareOp = VkCompareOp(dc.stencilfunc);
      depth_stencil.front.failOp = VkStencilOp(dc.stencilfail);
      depth_stencil.front.passOp = VkStencilOp(dc.stencilzpass);
      depth_stencil.front.depthFailOp = VkStencilOp(dc.stencilzfail);
      if (dc.backface_enable) {
        depth_stencil.back.compareOp = VkCompareOp(dc.stencilfunc_bf);
        depth_stencil.back.failOp = VkStencilOp(dc.stencilfail_bf);
        depth_stencil.back.passOp = VkStencilOp(dc.stencilzpass_bf);
        depth_stencil.back.depthFailOp = VkStencilOp(dc.stencilzfail_bf);
      } else {
        depth_stencil.back = depth_stencil.front;
      }
    }
    VkPipelineColorBlendAttachmentState blend[4] = {};
    for (uint32_t i = 0; i < color_count; ++i) {
      reg::RB_BLENDCONTROL bc;
      bc.value = key.blend[i];
      blend[i].colorWriteMask = (color_mask >> (4 * i)) & 0xF;
      blend[i].srcColorBlendFactor = BlendFactor(bc.color_srcblend);
      blend[i].dstColorBlendFactor = BlendFactor(bc.color_destblend);
      blend[i].colorBlendOp = BlendOp(bc.color_comb_fcn);
      blend[i].srcAlphaBlendFactor = BlendFactor(bc.alpha_srcblend);
      blend[i].dstAlphaBlendFactor = BlendFactor(bc.alpha_destblend);
      blend[i].alphaBlendOp = BlendOp(bc.alpha_comb_fcn);
      blend[i].blendEnable = !(blend[i].srcColorBlendFactor == VK_BLEND_FACTOR_ONE &&
                               blend[i].dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                               blend[i].colorBlendOp == VK_BLEND_OP_ADD &&
                               blend[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                               blend[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO &&
                               blend[i].alphaBlendOp == VK_BLEND_OP_ADD);
      if (!colors[i]) blend[i].colorWriteMask = 0;
    }
    VkPipelineColorBlendStateCreateInfo color_blend = {};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = color_count;
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
    for (uint32_t i = 0; i < color_count; ++i) color_formats[i] = VkFormat(key.color_formats[i]);
    VkPipelineRenderingCreateInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = color_count;
    rendering.pColorAttachmentFormats = color_formats;
    rendering.depthAttachmentFormat = depth ? depth->format : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = depth ? depth->format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = ps_entry ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = draw_pipeline_layout_;
    if (dfn.vkCreateGraphicsPipelines(device->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
      pipelines_.erase(pipeline_hash);
      REXGPU_ERROR("[sdnx_native] pipeline creation failed (vs {}, ps {})", vs_entry->name,
                   ps_entry ? ps_entry->name : "-");
      ++stats_.skipped;
      return true;
    }
  }

  if (log_frame_) {
    uint32_t base0 = 0, fmt0 = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!((color_mask >> (4 * i)) & 0xF)) continue;
      auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      base0 = info.color_base;
      fmt0 = uint32_t(info.color_format);
      break;
    }
    char textures[700] = {};
    int off = 0;
    for (uint32_t s = 0; s < 16 && off < 640; ++s) {
      if (shared[s] || shared[16 + s] || shared[32 + s]) {
        xenos::xe_gpu_texture_fetch_t f = regs.GetTextureFetch(s);
        off += snprintf(textures + off, sizeof(textures) - off, " t%u=%#x/f%u/d%u/%ux%u/i%u/s%u", s,
                        uint32_t(f.base_address) << 12, uint32_t(f.format), uint32_t(f.dimension),
                        uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1, (shared[s] | shared[16 + s] | shared[32 + s]) & 0xFFFFFF,
                        shared[48 + s]);
      }
    }
    REXGPU_INFO("[sdnx_frame] viewport {},{} {}x{} ndc scale {:.4f},{:.4f} offset {:.4f},{:.4f} area {}x{} "
                "vte {:#x} window_offset {:#x}",
                viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0], viewport.xy_extent[1],
                viewport.ndc_scale[0], viewport.ndc_scale[1], viewport.ndc_offset[0], viewport.ndc_offset[1], area_w,
                area_h, regs[XE_GPU_REG_PA_CL_VTE_CNTL], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET]);
    REXGPU_INFO("[sdnx_frame] draw {} prim={} n={} rt base={} fmt={} pitch={} mask={:#x} blend={:#x} depthctl={:#x} "
                "stencil={:#x} scissor={},{} {}x{} depth_img={:#x} {}x{} msaa={} binds={} rects={} vs={} ps={}{}",
                log_draw_, uint32_t(prim_type), index_count, base0, fmt0, pitch, color_mask,
                regs[reg::RB_BLENDCONTROL::rt_register_indices[0]], depth_control.value,
                regs[XE_GPU_REG_RB_STENCILREFMASK], scissor.offset[0], scissor.offset[1], scissor.extent[0],
                scissor.extent[1], uint64_t(reinterpret_cast<uintptr_t>(depth ? depth->image : 0)),
                depth ? depth->width : 0, depth ? depth->height : 0, uint32_t(surface.msaa_samples), binding_count,
                rect_count, vs_entry->name.substr(vs_entry->name.find_last_of("/\\") + 1),
                ps_entry ? ps_entry->name.substr(ps_entry->name.find_last_of("/\\") + 1) : "-", textures);
    ++log_draw_;
  }
  prof_mark(5);
  // EDRAM aliasing of a depth-only clear. Depth and colour share EDRAM: a depth + stencil write at the base of a
  // colour surface overwrites its samples with the depth word, (24-bit depth << 8) | stencil. Sleeping Dogs clears
  // the 600x352 ambient term of the final composite (0xED7B000) this way: a colour rectangle for x 560-600, then a
  // depth-only 4X rectangle of 280x176 (samples 0-560, the same tiles) with z 1 and stencil 0xFF -> 0xFFFFFFFF,
  // white. Images are separate here, so the left 560 columns kept 0 and the building facades lost their light
  // everywhere but a strip at the right edge. Only full writes of a known word are mirrored: a rectangle list
  // in window coordinates, z written with ALWAYS, stencil replaced with ALWAYS and the whole write mask.
  if (!color_pass && depth && rect_count && rect_bounds[0] < rect_bounds[2] && depth_control.z_write_enable &&
      depth_control.zfunc == xenos::CompareFunction::kAlways && depth_control.stencil_enable &&
      depth_control.stencilfunc == xenos::CompareFunction::kAlways &&
      depth_control.stencilzpass == xenos::StencilOp::kReplace && !(regs[XE_GPU_REG_PA_CL_VTE_CNTL] & 0x3F)) {
    auto ref = regs.Get<reg::RB_STENCILREFMASK>();
    if (ref.stencilwritemask == 0xFF) {
      float z = rect_has_z ? std::clamp(rect_z, 0.0f, 1.0f) : 1.0f;
      // In double and clamped: z 1 in float rounds 0xFFFFFF + 0.5 up to 2^24, which overflowed the word to 0.
      uint32_t depth24 = std::min(uint32_t(double(z) * double(0xFFFFFF) + 0.5), uint32_t(0xFFFFFF));
      uint32_t word = (depth24 << 8) | (ref.stencilref & 0xFF);
      VkClearAttachment clear = {};
      clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      for (int i = 0; i < 4; ++i) clear.clearValue.color.float32[i] = float((word >> (8 * i)) & 0xFF) / 255.0f;
      uint32_t depth_base = regs.Get<reg::RB_DEPTH_INFO>().depth_base;
      uint64_t base_pitch = uint64_t(depth_base & 0xFFF) | (uint64_t((pitch << msaa_sx) & 0xFFFF) << 12);
      int32_t x0 = std::max(int32_t(rect_bounds[0] + 0.5f), int32_t(scissor.offset[0])) << msaa_sx;
      int32_t y0 = std::max(int32_t(rect_bounds[1] + 0.5f), int32_t(scissor.offset[1])) << msaa_sy;
      int32_t x1 = std::min(int32_t(rect_bounds[2] + 0.5f), int32_t(scissor.offset[0] + scissor.extent[0])) << msaa_sx;
      int32_t y1 = std::min(int32_t(rect_bounds[3] + 0.5f), int32_t(scissor.offset[1] + scissor.extent[1])) << msaa_sy;
      for (auto& it : render_targets_) {
        Image& target = it.second;
        bool is_depth = (it.first >> 38) & 1;
        if (is_depth || (it.first & 0xFFFFFFF) != base_pitch || !target.image) continue;
        if (target.format != VK_FORMAT_R8G8B8A8_UNORM && target.format != VK_FORMAT_R8G8B8A8_SRGB) continue;
        int32_t cx1 = std::min(x1, int32_t(target.width)), cy1 = std::min(y1, int32_t(target.height));
        if (x0 < 0 || y0 < 0 || cx1 <= x0 || cy1 <= y0) continue;
        EndRendering();
        FullBarrier();
        VkRenderingAttachmentInfo color_info = {};
        color_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_info.imageView = target.view;
        color_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color_info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea = {{0, 0}, {target.width, target.height}};
        rendering_info.layerCount = 1;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_info;
        dfn.vkCmdBeginRendering(cb_, &rendering_info);
        VkClearRect rect = {{{x0, y0}, {uint32_t(cx1 - x0), uint32_t(cy1 - y0)}}, 0, 1};
        dfn.vkCmdClearAttachments(cb_, 1, &clear, 1, &rect);
        dfn.vkCmdEndRendering(cb_);
        target.drawn = true;
        ++stats_.clears;
        if (log_frame_) {
          REXGPU_INFO("[sdnx_frame] depth clear aliases colour base {} pitch {}: samples {},{}-{},{} word {:#010x}{}",
                      depth_base, pitch << msaa_sx, x0, y0, cx1, cy1, word, rect_has_z ? "" : " (z assumed 1)");
        }
      }
    }
  }
  // Record.
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: pipeline ready, begin recording", trace_index);
  }
  for (uint32_t i = 0; i < color_count; ++i) {
    if (colors[i]) colors[i]->drawn = true;
  }
  last_color_rt_ = color_count ? colors[0] : nullptr;
  if (depth) depth->drawn = true;
  if (!rendering_ || rendering_key_ != rendering_key) {
    EndRendering();
    FullBarrier();
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
    rendering_info.renderArea = {{0, 0}, {area_w, area_h}};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = color_count;
    rendering_info.pColorAttachments = color_infos;
    rendering_info.pDepthAttachment = depth ? &depth_info : nullptr;
    rendering_info.pStencilAttachment = depth ? &depth_info : nullptr;
    dfn.vkCmdBeginRendering(cb_, &rendering_info);
    rendering_ = true;
    rendering_key_ = rendering_key;
    BeginHostQuery();  // continues an open guest occlusion query in this pass
  }
  // Only what changed since the previous draw of this command buffer (bound_).
  if (bound_.pipeline != pipeline) {
    dfn.vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    bound_.pipeline = pipeline;
  }
  VkViewport vk_viewport = {float(viewport.xy_offset[0]), float(viewport.xy_offset[1]), float(viewport.xy_extent[0]),
                            float(viewport.xy_extent[1]), viewport.z_min, viewport.z_max};
  if (!bound_.viewport_set || std::memcmp(&bound_.viewport, &vk_viewport, sizeof(vk_viewport))) {
    dfn.vkCmdSetViewport(cb_, 0, 1, &vk_viewport);
    bound_.viewport = vk_viewport;
    bound_.viewport_set = true;
  }
  uint32_t sx = std::min(scissor.offset[0] << msaa_sx, area_w), sy = std::min(scissor.offset[1] << msaa_sy, area_h);
  VkRect2D vk_scissor = {{int32_t(sx), int32_t(sy)},
                         {std::min(scissor.extent[0] << msaa_sx, area_w - sx),
                          std::min(scissor.extent[1] << msaa_sy, area_h - sy)}};
  if (!bound_.scissor_set || std::memcmp(&bound_.scissor, &vk_scissor, sizeof(vk_scissor))) {
    dfn.vkCmdSetScissor(cb_, 0, 1, &vk_scissor);
    bound_.scissor = vk_scissor;
    bound_.scissor_set = true;
  }
  float blend_constants[4] = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                              regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  if (std::memcmp(bound_.blend, blend_constants, sizeof(blend_constants))) {
    dfn.vkCmdSetBlendConstants(cb_, blend_constants);
    std::memcpy(bound_.blend, blend_constants, sizeof(blend_constants));
  }
  {
    auto front = regs.Get<reg::RB_STENCILREFMASK>();
    reg::RB_STENCILREFMASK back = front;
    if (depth_control.backface_enable) back.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
    const uint32_t stencil[6] = {front.stencilref,  back.stencilref,       front.stencilmask,
                                 back.stencilmask,  front.stencilwritemask, back.stencilwritemask};
    if (stencil[0] != bound_.stencil[0]) dfn.vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_BIT, stencil[0]);
    if (stencil[1] != bound_.stencil[1]) dfn.vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_BACK_BIT, stencil[1]);
    if (stencil[2] != bound_.stencil[2]) dfn.vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_FRONT_BIT, stencil[2]);
    if (stencil[3] != bound_.stencil[3]) dfn.vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_BACK_BIT, stencil[3]);
    if (stencil[4] != bound_.stencil[4]) dfn.vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_FRONT_BIT, stencil[4]);
    if (stencil[5] != bound_.stencil[5]) dfn.vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_BACK_BIT, stencil[5]);
    std::memcpy(bound_.stencil, stencil, sizeof(stencil));
  }
  if (depth) {
    // Polygon offset, as the SDK Vulkan backend (host render target path).
    float bias_constant = 0.0f, bias_slope = 0.0f;
    draw_util::GetPreferredFacePolygonOffset(regs, draw_util::IsPrimitivePolygonal(regs), bias_slope, bias_constant);
    bias_constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
                         ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                         : draw_util::kD3D10PolygonOffsetFactorFloat24;
    bias_slope *= xenos::kPolygonOffsetScaleSubpixelUnit * float(1u << std::max(msaa_sx, msaa_sy));
    if (bias_constant != bound_.bias[0] || bias_slope != bound_.bias[1]) {
      dfn.vkCmdSetDepthBias(cb_, bias_constant, 0.0f, bias_slope);
      bound_.bias[0] = bias_constant;
      bound_.bias[1] = bias_slope;
    }
  }
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: binding descriptors (heaps + constants)", trace_index);
  }
  if (!bound_.heaps) {
    // The bindless heaps never change within a command buffer.
    dfn.vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline_layout_, 0, 4, heap_sets_, 0,
                                nullptr);
    bound_.heaps = true;
  }
  uint32_t dynamic_offsets[3] = {uint32_t(vs_constants), uint32_t(ps_constants), uint32_t(shared_constants)};
  VkDescriptorSet constants_set = constants_sets_[uint32_t(frame_ % kFramesInFlight)];
  // Same offsets as the previous draw of this command buffer (reused constants): the set stays bound.
  static VkCommandBuffer constants_cb = VK_NULL_HANDLE;
  static uint64_t constants_frame = UINT64_MAX;
  static uint32_t constants_offsets[3] = {};
  if (constants_cb != cb_ || constants_frame != frame_ || std::memcmp(constants_offsets, dynamic_offsets, 12) != 0) {
    dfn.vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline_layout_, 4, 1, &constants_set, 3,
                                dynamic_offsets);
    constants_cb = cb_;
    constants_frame = frame_;
    std::memcpy(constants_offsets, dynamic_offsets, 12);
  } else {
    ++stats_.constants_bind_skipped;
  }
  if (binding_count) {
    // Rebind from the first binding that differs.
    uint32_t first = 0;
    while (first < binding_count && first < bound_.vertex_count && bound_.vertex_buffers[first] == vb_buffers[first] &&
           bound_.vertex_offsets[first] == vb_offsets[first]) {
      ++first;
    }
    if (first < binding_count) {
      dfn.vkCmdBindVertexBuffers(cb_, first, binding_count - first, vb_buffers + first, vb_offsets + first);
    }
    for (uint32_t b = 0; b < binding_count; ++b) {
      bound_.vertex_buffers[b] = vb_buffers[b];
      bound_.vertex_offsets[b] = vb_offsets[b];
    }
    bound_.vertex_count = std::max(bound_.vertex_count, binding_count);
  }
  if (tr) {
    REXGPU_INFO("[sdnx_trace] draw {}: issuing {} {}", trace_index,
                index_draw_count ? index_draw_count : index_count, index_draw_count ? "cmdDrawIndexed" : "cmdDraw");
    ++trace_index;
  }
  if (index_draw_count) {
    if (bound_.index_buffer != index_buffer || bound_.index_offset != index_offset) {
      dfn.vkCmdBindIndexBuffer(cb_, index_buffer, index_offset, VK_INDEX_TYPE_UINT32);
      bound_.index_buffer = index_buffer;
      bound_.index_offset = index_offset;
    }
    dfn.vkCmdDrawIndexed(cb_, index_draw_count, 1, 0, vertex_offset, 0);
  } else {
    dfn.vkCmdDraw(cb_, index_count, 1, 0, 0);
  }
  ++stats_.drawn;
  if (prof) {
    prof_mark(6);
    for (int i = 0; i < 6; ++i) stats_.prof_ns[i] += prof_t[i + 1] - prof_t[i];
    stats_.prof_sub[0] += prof_t[7] - prof_t[1];  // vertices: fetch pairing
    stats_.prof_sub[1] += prof_t[8] - prof_t[7];  // vertices: frame + pulled fetches
    stats_.prof_sub[2] += prof_t[2] - prof_t[8];  // vertices: bindings
    ++stats_.prof_draws;
  }
  return true;
}

}  // namespace rex::graphics::sdnx





