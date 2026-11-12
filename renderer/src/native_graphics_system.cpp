/**
 * Sleeping Dogs port (SDNX): native renderer backend, graphics system.
 */
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/graphics/sdnx/native_command_processor.h>
#include <rex/graphics/sdnx/native_graphics_system.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/vulkan/provider.h>

REXCVAR_DEFINE_BOOL(sdnx_native_renderer, false, "GPU",
                    "Sleeping Dogs native renderer instead of the Xenos emulation (also SDNX_NATIVE=1)");

namespace rex::graphics::sdnx {

bool NativeRendererRequested() {
  const char* env = std::getenv("SDNX_NATIVE");
  if (env && *env && *env != '0') {
    return true;
  }
  return REXCVAR_GET(sdnx_native_renderer);
}

NativeGraphicsSystem::NativeGraphicsSystem() {}
NativeGraphicsSystem::~NativeGraphicsSystem() {}

std::string NativeGraphicsSystem::name() const {
  return "SDNX native (Vulkan)";
}

void NativeGraphicsSystem::CreateProvider(bool with_presentation) {
  // The XenosRecomp SPIR-V needs device features requested at creation (bindless arrays, int64, ...).
  rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
  provider_ = rex::ui::vulkan::VulkanProvider::Create(true, with_presentation);
}

std::unique_ptr<CommandProcessor> NativeGraphicsSystem::CreateCommandProcessor() {
  REXGPU_INFO("[sdnx_native] native renderer: no Xenos draw emulation (stage 2 skeleton)");
  return std::make_unique<NativeCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::sdnx
