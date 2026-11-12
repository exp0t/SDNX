/**
 * Sleeping Dogs port (SDNX): native renderer backend, skeleton (docs/native-renderer-plan.md in the port).
 *
 * The SDK's CommandProcessor keeps reading the PM4 ring (packets, registers, waits, interrupts, events); only
 * the expensive parts of the Xenos emulation are replaced: draws, resolves, swap, shader loads. No write-watched
 * shared memory, no run-time shader translation. Selected by the cvar sdnx_native_renderer or SDNX_NATIVE=1.
 */
#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::sdnx {

// True when the native renderer is requested (cvar sdnx_native_renderer or environment SDNX_NATIVE=1; the
// cvar may not reach a plugin DLL on the desktop, the environment always does).
bool NativeRendererRequested();

class NativeGraphicsSystem : public GraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::sdnx
