// SDNX deko3d shader package (nx/shaders/tools/dk_build.py): <dir>/index.txt, <dir>/textures.txt, <dir>/<name>.dksh.
// index.txt is the Vulkan package's index as it is: "<ps|vs> <key hex> <name> [vertex fetch order ...]" (the ps key
// is the raw microcode XXH3, the vs key the XXH3 with the vertex fetches zeroed). textures.txt gives each shader's
// texture bindings: "<name> <binding>:<2d|3d|cube>:<Xenos fetch slot> ...". Plain C++: no deko3d here.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sdnx::dk {

enum class TextureType : uint8_t { k2D, k3D, kCube };

struct PackageTexture {
  uint8_t binding = 0;  // deko3d texture binding of the shader's stage
  TextureType type = TextureType::k2D;
  uint8_t slot = 0;     // Xenos fetch constant
};

struct PackageShader {
  bool vertex = false;
  uint64_t key = 0;
  std::string name;
  std::string rest;  // the rest of the index line (vertex fetch order, "pull")
  std::vector<PackageTexture> textures;
};

class Package {
 public:
  // Reads index.txt and textures.txt. false with `error` set if either is missing or malformed.
  bool Open(const std::string& dir, std::string* error);
  const std::vector<PackageShader>& shaders() const { return shaders_; }
  const PackageShader* Find(bool vertex, uint64_t key) const;
  // The DKSH of a shader; empty if it cannot be read.
  std::vector<uint8_t> ReadDksh(const PackageShader& shader) const;

 private:
  std::string dir_;
  std::vector<PackageShader> shaders_;
  std::unordered_map<uint64_t, size_t> by_key_[2];  // [vertex]
};

}  // namespace sdnx::dk
