// Sleeping Dogs (Xbox 360 retail) for Nintendo Switch: the ReXGlue application object.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

REXCVAR_DECLARE(bool, sdretail_verify_data);           // sdnx_verify_data.cpp
void SdnxVerifyData(const std::filesystem::path& root);  // sdnx_verify_data.cpp

#ifdef __SWITCH__
// libnx svcGetInfo, declared here: switch.h clashes with the SDK types in this header.
extern "C" uint32_t svcGetInfo(uint64_t* out, uint32_t id0, uint32_t handle, uint64_t id1);
#endif

class SdRetailApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SdRetailApp>(new SdRetailApp(ctx, "sdretail", PPCImageConfig));
  }

  void OnPostInitLogging() override {
    // Data checksums: here and not in OnConfigurePaths, which runs before sdretail.toml is loaded (the flag set in
    // the file was not seen there on the console, 2026-11-11).
    if (REXCVAR_GET(sdretail_verify_data) && !game_data_root().empty()) {
      SdnxVerifyData(game_data_root());
    }
#ifdef __SWITCH__
    // Memory the process was given (InfoType_TotalMemorySize = 6, UsedMemorySize = 7, current process handle).
    // Started as an applet (hbmenu from the album) it is ~400 MB and the game cannot run: the guest's
    // physical memory alone is 512 MB. First run on a console (2026-11-10) died that way, with nothing in the
    // log that said why. Goes to stderr (rex/rex_stderr.log) and to the log.
    {
      uint64_t total = 0, used = 0;
      svcGetInfo(&total, 6, 0xFFFF8001u, 0);
      svcGetInfo(&used, 7, 0xFFFF8001u, 0);
      std::fprintf(stderr, "[memory] process memory: %llu MB total, %llu MB used at start\n",
                   (unsigned long long)(total >> 20), (unsigned long long)(used >> 20));
      if (total && total < (uint64_t(1500) << 20)) {
        std::fprintf(stderr,
                     "[memory] NOT ENOUGH MEMORY: this is applet mode (hbmenu opened from the album). Start hbmenu "
                     "over a game instead: hold R while launching any installed game.\n");
      }
    }
#endif
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation, preserving an
    // explicit command-line or config-file choice.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    // Face buttons by letter (the SDK default, input_xbox_layout = false): Switch A (right) acts as Xbox A
    // ("accept"), B (bottom) as B, X as X, Y as Y - what Switch players expect (user's choice, 2026-11-10). The
    // glyphs must then be patched with port/tools/apply_branding.py --by-letter. input_xbox_layout = true in
    // sdretail.toml gives the layout by position (then patch the data without --by-letter).
  }

  // The Switch has no command line: when --game_data_root is not given, use the game_root/ folder
  // next to the executable (sdmc:/switch/sdretail/game_root on the SD card).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    ResolveGameRoot(paths);
  }

  void ResolveGameRoot(rex::PathConfig& paths) {
    // On Horizon GetExecutableFolder() is the application path without the device ("/switch/sdretail"),
    // checked in Eden 2026-11-07; the sdmc: fallback below covers loaders that give no path at all.
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    if (!folder.empty()) {
      const std::filesystem::path extracted = folder / "game_root";
      if (std::filesystem::is_directory(extracted, ec)) {
        paths.game_data_root = extracted;
        return;
      }
    }
    static constexpr const char* kFallbackRoot = "sdmc:/switch/sdretail/game_root";
    const std::string probe = std::string(kFallbackRoot) + "/default.xex";
    if (std::FILE* f = std::fopen(probe.c_str(), "rb")) {
      std::fclose(f);
      paths.game_data_root = std::filesystem::path(kFallbackRoot);
    }
  }
};

