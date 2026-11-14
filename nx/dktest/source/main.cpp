// SDNX deko3d test NRO: phase 1 of the deko3d backend. It checks on the console what the PC bench cannot, with
// the deko3d core the renderer will use (nx/deko3d, from rexglue-sdk-nx) and shaders made by the same
// HLSL -> SPIR-V -> GLSL -> uam path as the game library (nx/shaders/tools/dk_probe.py). Steps, each logged:
//   1. device, queue (ZCULL on), work slots, swapchain; 2 s of cleared frames (the screen changes colour);
//   2. the test shaders on an offscreen image, read back and compared with the expected colours: UBO bindings 0-2
//      of the game layout, the slot 0 texture and sampler, the orientation (row 0 = top) - PASS or FAIL;
//   3. draw cost: 500 / 2000 / 5000 draws per frame, each with its own UBOs and texture bind, 3 s each: CPU ns per
//      recorded draw and GPU ms per frame (timestamps);
//   4. every game DKSH: loaded (dkShaderIsValid), then drawn once (pixel shaders with the test vertex shader,
//      vertex shaders with the test pixel shader) with zero constants and a texture of the right type at every
//      binding, in batches with a wait after each: a GPU fault names its batch in the log.
// The log is dktest.log next to the NRO. The last screen is green if everything passed, red otherwise; + quits.

#include <switch.h>
#include <deko3d.h>

#include <rex/ui/deko3d/nucleo.h>
#include <sdnx/dk_package.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rd = rex::deko3d;

namespace {

FILE* g_log = nullptr;
void WriteLine(const char* line) {
  if (g_log) {
    std::fputs(line, g_log);
    std::fputc('\n', g_log);
  }
}
void FlushLog() {
  if (g_log) {
    std::fflush(g_log);
  }
}
#define LOG(...) rd::Registro(__VA_ARGS__)

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  data.resize(size_t(std::ftell(f)));
  std::fseek(f, 0, SEEK_SET);
  if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
    data.clear();
  }
  std::fclose(f);
  return data;
}

constexpr uint32_t kZeroBytes = 64 << 10;  // zero constants, storage buffer and vertices for the game shaders
// A shared block at this offset of the same memory with the alpha test on (dk mode: the word at byte 616 is the
// pipeline's specialization value, shader_common.h): every other game pixel shader is drawn with it.
constexpr uint32_t kAlphaShared = 60 << 10;
constexpr uint32_t kOffscreen = 64;        // the readback image of step 2

enum ImageId : uint32_t { kImgQuadrants = 0, kImgVolume = 1, kImgCube = 2, kImgCount = 3 };
enum SamplerId : uint32_t { kSmpNearestClamp = 0, kSmpLinearRepeat = 1 };

struct GameShader {
  const sdnx::dk::PackageShader* entry = nullptr;  // name, stage, texture bindings (textures.txt)
  uint32_t ssbos = 0, inputs = 0;                   // dksh_test.txt
  std::vector<uint8_t> dksh;  // kept: the control section stays in CPU memory
  DkShader shader{};
  bool loaded = false;
};

class Test {
 public:
  ~Test() {
    if (zeros_) {
      nucleo_.EsperarGpu();
      disp_.DestruirBloque(zeros_);
    }
  }
  int Run(const std::string& dir);

 private:
  struct Image {
    DkImage image{};
    rd::BloqueImagen memory;
  };

  void CreateImage(Image& img, DkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t flags);
  bool LoadShader(DkShader& s, const char* path);
  // Opens a work and binds what every draw here shares. `target` null: the swapchain image `*image_out`.
  DkCmdBuf BeginFrame(int* image_out);
  void EndFrame(int image);
  void BindTarget(DkCmdBuf c, DkImage& image, uint32_t w, uint32_t h);
  void DefaultState(DkCmdBuf c);
  // Per-draw UBOs from the upload ring: the rectangle (vertex binding 0) and the pixel tint (fragment binding 1).
  bool DrawRect(DkCmdBuf c, float left, float bottom, float right, float top, const float tint[4]);
  bool BindShared(DkCmdBuf c, const float tint[4]);
  void BindForGame(DkCmdBuf c, const GameShader& g, DkStage stage);

  bool StepClear();
  bool StepReadback();
  void StepDrawCost(uint32_t draws);
  bool StepGameShaders();
  void FinalScreen(bool ok);

  rd::Dispositivo disp_;
  rd::Nucleo nucleo_;
  rd::Presentacion present_;
  uint64_t work_ = 0;
  DkShader test_vs_{}, test_ps_{};
  Image images_[kImgCount];
  Image offscreen_;
  DkMemBlock zeros_ = nullptr;
  DkGpuAddr zeros_gpu_ = 0;
  DkMemBlock readback_ = nullptr;
  sdnx::dk::Package package_;
  std::vector<GameShader> game_;
};

void Test::CreateImage(Image& img, DkImageType type, uint32_t w, uint32_t h, uint32_t d, uint32_t flags) {
  DkImageLayoutMaker m;
  dkImageLayoutMakerDefaults(&m, nucleo_.dispositivo());
  m.type = type;
  m.flags = flags;
  m.format = DkImageFormat_RGBA8_Unorm;
  m.dimensions[0] = w;
  m.dimensions[1] = h;
  m.dimensions[2] = type == DkImageType_Cubemap ? 6 : d;
  rd::ArreglarTamanoDeTesela(m, h);
  DkImageLayout layout;
  dkImageLayoutInitialize(&layout, &m);
  img.memory = nucleo_.ReservarImagen(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
  dkImageInitialize(&img.image, &layout, img.memory.bloque, img.memory.offset);
}

bool Test::LoadShader(DkShader& s, const char* path) {
  static std::vector<std::vector<uint8_t>> keep;  // the control sections live as long as the shaders
  keep.push_back(ReadFile(path));
  if (keep.back().empty()) {
    LOG("[test] FAIL: cannot read %s", path);
    return false;
  }
  return nucleo_.CargarShader(s, keep.back(), path);
}

DkCmdBuf Test::BeginFrame(int* image_out) {
  nucleo_.EmpezarTrabajo(work_);
  DkCmdBuf c = nucleo_.comandos();
  nucleo_.BindDescriptores();
  nucleo_.InvalidarCodigoSiHaceFalta();
  if (image_out) {
    *image_out = present_.Adquirir();
  }
  return c;
}

void Test::EndFrame(int image) {
  nucleo_.TerminarTrabajo();
  if (image >= 0) {
    present_.Presentar(image);
  }
  ++work_;
}

void Test::BindTarget(DkCmdBuf c, DkImage& image, uint32_t w, uint32_t h) {
  DkImageView view;
  dkImageViewDefaults(&view, &image);
  dkCmdBufBindRenderTarget(c, &view, nullptr);
  const DkViewport vp = {0, 0, float(w), float(h), 0, 1};
  const DkScissor sc = {0, 0, w, h};
  dkCmdBufSetViewports(c, 0, &vp, 1);
  dkCmdBufSetScissors(c, 0, &sc, 1);
}

void Test::DefaultState(DkCmdBuf c) {
  DkRasterizerState rs;
  dkRasterizerStateDefaults(&rs);
  rs.cullMode = DkFace_None;
  DkColorState cs;
  dkColorStateDefaults(&cs);
  DkColorWriteState cws;
  dkColorWriteStateDefaults(&cws);
  DkDepthStencilState ds;
  dkDepthStencilStateDefaults(&ds);
  ds.depthTestEnable = false;
  ds.depthWriteEnable = false;
  DkBlendState bs;
  dkBlendStateDefaults(&bs);
  dkCmdBufBindRasterizerState(c, &rs);
  dkCmdBufBindColorState(c, &cs);
  dkCmdBufBindColorWriteState(c, &cws);
  dkCmdBufBindDepthStencilState(c, &ds);
  dkCmdBufBindBlendState(c, 0, &bs);
  dkCmdBufBindVtxAttribState(c, nullptr, 0);
  dkCmdBufBindVtxBufferState(c, nullptr, 0);
}

bool Test::BindShared(DkCmdBuf c, const float tint[4]) {
  // The game's shared block is 59 float4; the test reads the last one, so a short binding shows up.
  const rd::Subida s = nucleo_.Reservar(59 * 16, DK_UNIFORM_BUF_ALIGNMENT);
  if (!s) {
    return false;
  }
  std::memset(s.cpu, 0, 59 * 16);
  std::memcpy(s.cpu + 58 * 16, tint, 16);
  dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 2, s.gpu, 59 * 16);
  return true;
}

bool Test::DrawRect(DkCmdBuf c, float left, float bottom, float right, float top, const float tint[4]) {
  const rd::Subida s = nucleo_.Reservar(2 * DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
  if (!s) {
    return false;
  }
  const float rect[4] = {left, bottom, right, top};
  std::memcpy(s.cpu, rect, 16);
  std::memcpy(s.cpu + DK_UNIFORM_BUF_ALIGNMENT, tint, 16);
  dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, s.gpu, 16);
  dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 1, s.gpu + DK_UNIFORM_BUF_ALIGNMENT, 16);
  dkCmdBufBindTexture(c, DkStage_Fragment, 0, dkMakeTextureHandle(kImgQuadrants, kSmpNearestClamp));
  dkCmdBufDraw(c, DkPrimitive_Triangles, 3, 1, 0, 0);
  return true;
}

bool Test::StepClear() {
  LOG("[test] step 1: cleared frames, 2 s");
  const uint64_t start = rd::AhoraNs();
  uint32_t frames = 0;
  while (appletMainLoop() && rd::AhoraNs() - start < 2'000'000'000ull) {
    int image;
    DkCmdBuf c = BeginFrame(&image);
    BindTarget(c, present_.Imagen(image), present_.ancho(), present_.alto());
    const float t = float(frames % 120) / 120.0f;
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, t, 0.2f, 1.0f - t, 1.0f);
    EndFrame(image);
    ++frames;
  }
  const double s = double(rd::AhoraNs() - start) * 1e-9;
  LOG("[test] step 1: %u frames in %.2f s (%.1f fps with vsync): OK", frames, s, frames / s);
  return frames > 0;
}

bool Test::StepReadback() {
  LOG("[test] step 2: test shaders on a %ux%u image, read back", kOffscreen, kOffscreen);
  DkCmdBuf c = BeginFrame(nullptr);
  BindTarget(c, offscreen_.image, kOffscreen, kOffscreen);
  dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0, 0, 0, 0);
  DefaultState(c);
  const DkShader* pair[2] = {&test_vs_, &test_ps_};
  dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, pair, 2);
  const float shared[4] = {1.0f, 1.0f, 0.5f, 1.0f};
  const float tint[4] = {1.0f, 0.5f, 1.0f, 1.0f};
  bool ok = BindShared(c, shared) && DrawRect(c, -1.0f, -1.0f, 1.0f, 1.0f, tint);
  dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image);
  DkImageView view;
  dkImageViewDefaults(&view, &offscreen_.image);
  const DkImageRect rect = {0, 0, 0, kOffscreen, kOffscreen, 1};
  const DkCopyBuf dst = {dkMemBlockGetGpuAddr(readback_), 0, 0};
  dkMemBlockFlushCpuCache(readback_, 0, kOffscreen * kOffscreen * 4);
  dkCmdBufCopyImageToBuffer(c, &view, &rect, &dst, 0);
  dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_L2Cache);
  EndFrame(-1);
  nucleo_.EsperarGpu();
  dkMemBlockFlushCpuCache(readback_, 0, kOffscreen * kOffscreen * 4);
  const uint8_t* px = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(readback_));
  // Texture quadrants red, green, blue, white (row 0 = top), times tint (1, .5, 1) and shared (1, 1, .5).
  struct Probe {
    const char* where;
    uint32_t x, y;
    int r, g, b;
  } probes[4] = {{"top-left", 16, 16, 255, 0, 0},
                 {"top-right", 48, 16, 0, 128, 0},
                 {"bottom-left", 16, 48, 0, 0, 128},
                 {"bottom-right", 48, 48, 255, 128, 128}};
  for (const Probe& p : probes) {
    const uint8_t* q = px + (p.y * kOffscreen + p.x) * 4;
    const bool match = std::abs(q[0] - p.r) <= 2 && std::abs(q[1] - p.g) <= 2 && std::abs(q[2] - p.b) <= 2;
    LOG("[test] step 2: %-12s (%u, %u) = %3u %3u %3u %3u, expected %3d %3d %3d: %s", p.where, p.x, p.y, q[0], q[1],
        q[2], q[3], p.r, p.g, p.b, match ? "ok" : "WRONG");
    ok &= match;
  }
  LOG("[test] step 2: %s", ok ? "PASS" : "FAIL");
  return ok;
}

void Test::StepDrawCost(uint32_t draws) {
  LOG("[test] step 3: %u draws per frame, 3 s", draws);
  const uint32_t grid = uint32_t(std::ceil(std::sqrt(double(draws))));
  const uint64_t start = rd::AhoraNs();
  uint64_t record_ns = 0, gpu_units = 0, frames = 0, gpu_frames = 0;
  // Timestamps per slot: start at +0, end at +16 (16-byte reports; the second word is the counter).
  const uint32_t slots = nucleo_.config().ranuras;
  std::vector<bool> pending(slots, false);
  while (appletMainLoop() && rd::AhoraNs() - start < 3'000'000'000ull) {
    const uint32_t slot = uint32_t(work_ % slots);
    int image;
    DkCmdBuf c = BeginFrame(&image);  // waits for this slot's previous work: its timestamps are written
    if (pending[slot]) {
      const uint8_t* q = nucleo_.ConsultasCpu() + slot * 32;
      uint64_t t0, t1;
      std::memcpy(&t0, q + 8, 8);
      std::memcpy(&t1, q + 16 + 8, 8);
      if (t1 > t0) {
        gpu_units += t1 - t0;
        ++gpu_frames;
      }
    }
    const DkGpuAddr reports = nucleo_.ConsultasGpu() + slot * 32;
    dkCmdBufReportCounter(c, DkCounter_Timestamp, reports);
    BindTarget(c, present_.Imagen(image), present_.ancho(), present_.alto());
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0.05f, 0.05f, 0.05f, 1.0f);
    DefaultState(c);
    const DkShader* pair[2] = {&test_vs_, &test_ps_};
    dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, pair, 2);
    const float one[4] = {1, 1, 1, 1};
    BindShared(c, one);
    const uint64_t t = rd::AhoraNs();
    const float cell = 2.0f / float(grid);
    for (uint32_t i = 0; i < draws; ++i) {
      const float x = -1.0f + float(i % grid) * cell, y = 1.0f - float(i / grid + 1) * cell;
      const float tint[4] = {float(i % 7) / 6.0f, float(i % 5) / 4.0f, float(i % 3) / 2.0f, 1.0f};
      if (!DrawRect(c, x, y, x + cell * 0.9f, y + cell * 0.9f, tint)) {
        break;
      }
    }
    record_ns += rd::AhoraNs() - t;
    dkCmdBufReportCounter(c, DkCounter_Timestamp, reports + 16);
    pending[slot] = true;
    EndFrame(image);
    ++frames;
  }
  const double s = double(rd::AhoraNs() - start) * 1e-9;
  LOG("[test] step 3: %u draws: %llu frames, %.1f fps (vsync), CPU %.0f ns per draw recorded (%.2f ms per frame), "
      "GPU %.2f ms per frame",
      draws, static_cast<unsigned long long>(frames), frames / s,
      frames ? double(record_ns) / double(frames * draws) : 0.0,
      frames ? double(record_ns) / double(frames) * 1e-6 : 0.0,
      gpu_frames ? double(gpu_units) * rd::kNsPorUnidadGpu / double(gpu_frames) * 1e-6 : 0.0);
  const rd::EstadisticasNucleo e = nucleo_.TomarEstadisticas();
  LOG("[test] step 3: upload %.2f MB per frame, command memory up to %llu KB per frame, %llu overflows",
      e.trabajos ? double(e.bytes_subida) / double(e.trabajos) / 1e6 : 0.0,
      static_cast<unsigned long long>(e.bytes_comandos_max >> 10),
      static_cast<unsigned long long>(e.desbordes_comandos));
}

void Test::BindForGame(DkCmdBuf c, const GameShader& g, DkStage stage) {
  // The package's table: each binding with a texture of its type (the Xenos slot is what the renderer will use).
  for (const sdnx::dk::PackageTexture& tex : g.entry->textures) {
    const uint32_t img = tex.type == sdnx::dk::TextureType::k3D     ? kImgVolume
                         : tex.type == sdnx::dk::TextureType::kCube ? kImgCube
                                                                    : kImgQuadrants;
    dkCmdBufBindTexture(c, stage, tex.binding, dkMakeTextureHandle(img, kSmpLinearRepeat));
  }
  for (uint32_t b = 0; b < 3; ++b) {
    dkCmdBufBindUniformBuffer(c, stage, b, zeros_gpu_, 4096);
  }
  if (g.ssbos & (1u << 3)) {
    dkCmdBufBindStorageBuffer(c, stage, 3, zeros_gpu_, kZeroBytes);
  }
}

bool Test::StepGameShaders() {
  LOG("[test] step 4: game shaders");
  std::string error;
  if (!package_.Open("romfs:/dksh", &error)) {
    LOG("[test] step 4: FAIL: %s", error.c_str());
    return false;
  }
  const std::vector<uint8_t> extra = ReadFile("romfs:/dksh_test.txt");
  std::string text(extra.begin(), extra.end());
  for (const sdnx::dk::PackageShader& s : package_.shaders()) {
    GameShader g;
    g.entry = &s;
    const size_t at = text.find(s.name + " ");
    if (at != std::string::npos) {
      std::sscanf(text.c_str() + at + s.name.size(), "%x %x", &g.ssbos, &g.inputs);
    }
    if (s.vertex && s.rest.find("pull") != std::string::npos && !(g.ssbos & 8)) {
      LOG("[test] step 4: %s pulls its vertices but reads no storage buffer 3", s.name.c_str());
    }
    game_.push_back(std::move(g));
  }
  uint32_t textures = 0;
  for (const GameShader& g : game_) {
    textures += uint32_t(g.entry->textures.size());
  }
  LOG("[test] step 4: package: %zu shaders, %u texture bindings", game_.size(), textures);
  uint64_t t = rd::AhoraNs();
  uint32_t loaded = 0, failed = 0, bytes = 0;
  for (GameShader& g : game_) {
    g.dksh = package_.ReadDksh(*g.entry);
    bytes += uint32_t(g.dksh.size());
    g.loaded = !g.dksh.empty() && nucleo_.CargarShader(g.shader, g.dksh, g.entry->name.c_str());
    if (g.loaded) {
      ++loaded;
    } else {
      ++failed;
      LOG("[test] step 4: %s did not load", g.entry->name.c_str());
    }
  }
  LOG("[test] step 4: %u of %zu loaded (%u failed), %.1f MB of DKSH, %.0f ms", loaded, game_.size(), failed,
      bytes / 1e6, double(rd::AhoraNs() - t) * 1e-6);

  // Draws, in batches of 64 with a wait after each: a GPU fault stops at its batch, logged before it is sent.
  constexpr size_t kBatch = 64;
  std::vector<const GameShader*> order;
  for (const GameShader& g : game_) {
    if (g.loaded && !g.entry->vertex) {
      order.push_back(&g);
    }
  }
  const size_t pixel_count = order.size();
  for (const GameShader& g : game_) {
    if (g.loaded && g.entry->vertex) {
      order.push_back(&g);
    }
  }
  t = rd::AhoraNs();
  const DkVtxBufferState vtx_buffer = {16, 0};
  std::vector<DkVtxAttribState> attribs(32);
  for (DkVtxAttribState& a : attribs) {
    a = DkVtxAttribState{0, 0, 0, DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0};
  }
  for (size_t first = 0; first < order.size(); first += kBatch) {
    const size_t last = std::min(order.size(), first + kBatch);
    LOG("[test] step 4: drawing %zu-%zu (%s ... %s)", first, last - 1, order[first]->entry->name.c_str(),
        order[last - 1]->entry->name.c_str());
    rd::VaciarRegistro();
    int image;
    DkCmdBuf c = BeginFrame(&image);
    BindTarget(c, present_.Imagen(image), present_.ancho(), present_.alto());
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, 0.1f, 0.1f, 0.3f, 1.0f);
    DefaultState(c);
    for (size_t i = first; i < last; ++i) {
      const GameShader& g = *order[i];
      const float x = -1.0f + float(i % 32) / 16.0f, y = -1.0f + float((i / 32) % 32) / 16.0f;
      if (!g.entry->vertex) {
        const DkShader* pair[2] = {&test_vs_, &g.shader};
        dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, pair, 2);
        BindForGame(c, g, DkStage_Fragment);
        if (i & 1) {
          dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 2, zeros_gpu_ + kAlphaShared, 59 * 16);
        }
        // The test vertex shader's own UBO (binding 0) after the zero ones.
        const rd::Subida s = nucleo_.Reservar(16, DK_UNIFORM_BUF_ALIGNMENT);
        if (!s) {
          break;
        }
        const float rect[4] = {x, y, x + 0.06f, y + 0.06f};
        std::memcpy(s.cpu, rect, 16);
        dkCmdBufBindUniformBuffer(c, DkStage_Vertex, 0, s.gpu, 16);
        dkCmdBufDraw(c, DkPrimitive_Triangles, 3, 1, 0, 0);
      } else {
        const DkShader* pair[2] = {&g.shader, &test_ps_};
        dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, pair, 2);
        uint32_t count = 0;
        for (uint32_t b = 0; b < 32; ++b) {
          if (g.inputs & (1u << b)) {
            count = b + 1;
          }
        }
        dkCmdBufBindVtxAttribState(c, attribs.data(), count);
        dkCmdBufBindVtxBufferState(c, &vtx_buffer, 1);
        dkCmdBufBindVtxBuffer(c, 0, zeros_gpu_, kZeroBytes);
        BindForGame(c, g, DkStage_Vertex);
        const float one[4] = {1, 1, 1, 1};
        BindShared(c, one);  // the test pixel shader's
        dkCmdBufBindUniformBuffer(c, DkStage_Fragment, 1, zeros_gpu_, 4096);
        dkCmdBufBindTexture(c, DkStage_Fragment, 0, dkMakeTextureHandle(kImgQuadrants, kSmpNearestClamp));
        dkCmdBufDraw(c, DkPrimitive_Triangles, 3, 1, 0, 0);
        dkCmdBufBindVtxAttribState(c, nullptr, 0);
        dkCmdBufBindVtxBufferState(c, nullptr, 0);
      }
    }
    EndFrame(image);
    nucleo_.EsperarGpu();
  }
  LOG("[test] step 4: drew %zu pixel and %zu vertex shaders in %.0f ms, no GPU fault: %s", pixel_count,
      order.size() - pixel_count, double(rd::AhoraNs() - t) * 1e-6, failed ? "FAIL (load)" : "PASS");
  return failed == 0;
}

void Test::FinalScreen(bool ok) {
  LOG("[test] all steps: %s. Press + to quit.", ok ? "PASS" : "FAIL");
  rd::VaciarRegistro();
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
      break;
    }
    int image;
    DkCmdBuf c = BeginFrame(&image);
    BindTarget(c, present_.Imagen(image), present_.ancho(), present_.alto());
    dkCmdBufClearColorFloat(c, 0, DkColorMask_RGBA, ok ? 0.0f : 0.8f, ok ? 0.6f : 0.0f, 0.0f, 1.0f);
    EndFrame(image);
  }
}

int Test::Run(const std::string& dir) {
  disp_.Crear(true);
  rd::ConfigNucleo config;
  config.ranuras = 3;
  config.mb_subida = 16;
  config.mb_codigo = 40;
  config.mb_trozo_imagenes = 4;
  config.descriptores_imagen = 64;
  config.descriptores_sampler = 16;
  nucleo_.Crear(disp_, config);
  uint32_t w, h;
  rd::TamanoDePantalla(w, h);
  present_.Crear(disp_, w, h, 1);
  LOG("[test] screen %ux%u", w, h);

  zeros_ = disp_.CrearBloque(kZeroBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "zeros");
  std::memset(dkMemBlockGetCpuAddr(zeros_), 0, kZeroBytes);
  {
    uint8_t* shared = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(zeros_)) + kAlphaShared;
    const uint32_t spec = 0x2u | (6u << 16);  // alpha test, >=
    const float threshold = 0.5f;
    std::memcpy(shared + 616, &spec, 4);
    std::memcpy(shared + 272, &threshold, 4);
  }
  zeros_gpu_ = dkMemBlockGetGpuAddr(zeros_);
  readback_ = nucleo_.CrearBloqueLectura(kOffscreen * kOffscreen * 4, "readback");

  bool ok = StepClear();
  if (!LoadShader(test_vs_, "romfs:/dktest_vs.dksh") || !LoadShader(test_ps_, "romfs:/dktest_ps.dksh")) {
    FinalScreen(false);
    return 1;
  }

  // Images and descriptors: the quadrant texture (uploaded), an empty 3D and cube texture for the game shaders.
  CreateImage(images_[kImgQuadrants], DkImageType_2D, 64, 64, 1, 0);
  CreateImage(images_[kImgVolume], DkImageType_3D, 4, 4, 4, 0);
  CreateImage(images_[kImgCube], DkImageType_Cubemap, 4, 4, 6, 0);
  CreateImage(offscreen_, DkImageType_2D, kOffscreen, kOffscreen, 1, DkImageFlags_UsageRender);
  {
    DkCmdBuf c = BeginFrame(nullptr);
    const rd::Subida s = nucleo_.Reservar(64 * 64 * 4, 256);
    for (uint32_t y = 0; y < 64; ++y) {
      for (uint32_t x = 0; x < 64; ++x) {
        uint8_t* p = s.cpu + (y * 64 + x) * 4;
        const bool right = x >= 32, bottom = y >= 32;
        const uint8_t r = (!right && !bottom) || (right && bottom) ? 255 : 0;
        const uint8_t g = (right && !bottom) || (right && bottom) ? 255 : 0;
        const uint8_t b = (!right && bottom) || (right && bottom) ? 255 : 0;
        p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
      }
    }
    DkImageView view;
    dkImageViewDefaults(&view, &images_[kImgQuadrants].image);
    const DkCopyBuf src = {s.gpu, 0, 0};
    const DkImageRect rect = {0, 0, 0, 64, 64, 1};
    dkCmdBufCopyBufferToImage(c, &src, &view, &rect, 0);
    for (uint32_t i = 0; i < kImgCount; ++i) {
      DkImageView v;
      dkImageViewDefaults(&v, &images_[i].image);
      DkImageDescriptor d;
      dkImageDescriptorInitialize(&d, &v, false, false);
      dkCmdBufPushData(c, nucleo_.DescriptoresImagen() + i * sizeof(d), &d, sizeof(d));
    }
    DkSampler smp[2];
    DkSamplerDescriptor sd[2];
    for (int i = 0; i < 2; ++i) {
      dkSamplerDefaults(&smp[i]);
      smp[i].minFilter = smp[i].magFilter = i == 0 ? DkFilter_Nearest : DkFilter_Linear;
      smp[i].wrapMode[0] = smp[i].wrapMode[1] = smp[i].wrapMode[2] = i == 0 ? DkWrapMode_ClampToEdge
                                                                             : DkWrapMode_Repeat;
      dkSamplerDescriptorInitialize(&sd[i], &smp[i]);
    }
    dkCmdBufPushData(c, nucleo_.DescriptoresSampler(), sd, sizeof(sd));
    dkCmdBufBarrier(c, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    EndFrame(-1);
    nucleo_.EsperarGpu();
    LOG("[test] textures and descriptors ready");
  }

  ok &= StepReadback();
  for (uint32_t draws : {500u, 2000u, 5000u}) {
    StepDrawCost(draws);
  }
  ok &= StepGameShaders();
  FinalScreen(ok);
  (void)dir;
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  romfsInit();
  std::string dir = "sdmc:/switch/sdnx_dktest";
  if (argc > 0 && argv[0] && std::strncmp(argv[0], "sdmc:/", 6) == 0) {
    dir = argv[0];
    dir = dir.substr(0, dir.rfind('/'));
  }
  g_log = std::fopen((dir + "/dktest.log").c_str(), "w");
  if (!g_log) {
    g_log = std::fopen("sdmc:/sdnx_dktest.log", "w");
  }
  rd::FijarRegistro(WriteLine, FlushLog);
  LOG("[test] SDNX deko3d test, built " __DATE__ " " __TIME__ ", log in %s", dir.c_str());
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  int rc;
  {
    Test test;
    rc = test.Run(dir);
  }
  LOG("[test] done");
  rd::VaciarRegistro();
  if (g_log) {
    std::fclose(g_log);
  }
  romfsExit();
  return rc;
}
