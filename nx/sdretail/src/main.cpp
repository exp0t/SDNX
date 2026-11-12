// sdretail (Sleeping Dogs, Xbox 360 retail) for Nintendo Switch: application entry point.

#include "generated/default/sdretail_init.h"

#include "sdretail_app.h"

#ifdef __SWITCH__
// Loaders that start the NRO without a program name (Ryujinx: argv[0] = null) would leave the SDK
// without the executable folder, where game_root/ and the configuration live. hbloader always passes it.
extern "C" int __system_argc;
extern "C" char** __system_argv;
#include <sys/iosupport.h>
extern "C" const devoptab_t dotab_stdnull;
namespace {
const bool kArgvByDefault = [] {
  static char path[] = "sdmc:/switch/sdretail/sdretail.nro";
  static char* argv[] = {path, nullptr};
  if (__system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0]) {
    __system_argc = 1;
    __system_argv = argv;
  }
  // The same loaders leave stdout/stderr without a device: any fprintf(stderr) faults.
  if (!devoptab_list[STD_OUT]) devoptab_list[STD_OUT] = &dotab_stdnull;
  if (!devoptab_list[STD_ERR]) devoptab_list[STD_ERR] = &dotab_stdnull;
  return true;
}();
}  // namespace
#endif

REX_DEFINE_APP(sdretail, SdRetailApp::Create)
