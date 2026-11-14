// SDNX deko3d shader package. See sdnx/dk_package.h.

#include <sdnx/dk_package.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sdnx::dk {
namespace {

bool ReadText(const std::string& path, std::string& text) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  char buffer[16384];
  size_t n;
  text.clear();
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
    text.append(buffer, n);
  }
  std::fclose(f);
  return true;
}

// Calls `line(text)` for each non-empty line, without its '\r'.
template <typename F>
void ForEachLine(const std::string& text, F&& line) {
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    size_t last = end;
    while (last > at && (text[last - 1] == '\r' || text[last - 1] == ' ')) {
      --last;
    }
    if (last > at) {
      line(text.substr(at, last - at));
    }
    at = end + 1;
  }
}

}  // namespace

bool Package::Open(const std::string& dir, std::string* error) {
  dir_ = dir;
  shaders_.clear();
  by_key_[0].clear();
  by_key_[1].clear();
  std::string index, textures;
  if (!ReadText(dir + "/index.txt", index) || !ReadText(dir + "/textures.txt", textures)) {
    if (error) {
      *error = "no index.txt or textures.txt in " + dir;
    }
    return false;
  }
  bool ok = true;
  std::unordered_map<std::string, size_t> by_name;
  ForEachLine(index, [&](const std::string& line) {
    char kind[4] = {}, key[24] = {}, name[64] = {};
    int used = 0;
    if (std::sscanf(line.c_str(), "%3s %23s %63s%n", kind, key, name, &used) != 3 ||
        (std::strcmp(kind, "vs") && std::strcmp(kind, "ps"))) {
      if (error && ok) {
        *error = "bad index line: " + line;
      }
      ok = false;
      return;
    }
    PackageShader s;
    s.vertex = kind[0] == 'v';
    s.key = std::strtoull(key, nullptr, 16);
    s.name = name;
    size_t rest = size_t(used);
    while (rest < line.size() && line[rest] == ' ') {
      ++rest;
    }
    s.rest = line.substr(rest);
    by_name[s.name] = shaders_.size();
    by_key_[s.vertex].emplace(s.key, shaders_.size());
    shaders_.push_back(std::move(s));
  });
  ForEachLine(textures, [&](const std::string& line) {
    const size_t space = line.find(' ');
    const auto it = by_name.find(line.substr(0, space));
    if (it == by_name.end()) {
      return;
    }
    PackageShader& s = shaders_[it->second];
    size_t at = space;
    while (at != std::string::npos && at < line.size()) {
      const size_t next = line.find(' ', at + 1);
      const std::string item = line.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1);
      unsigned binding = 0, slot = 0;
      char type[8] = {};
      if (std::sscanf(item.c_str(), "%u:%7[^:]:%u", &binding, type, &slot) == 3 && binding < 32 && slot < 32) {
        PackageTexture t;
        t.binding = uint8_t(binding);
        t.slot = uint8_t(slot);
        t.type = !std::strcmp(type, "3d") ? TextureType::k3D : !std::strcmp(type, "cube") ? TextureType::kCube
                                                                                          : TextureType::k2D;
        s.textures.push_back(t);
      } else if (!item.empty()) {
        if (error && ok) {
          *error = "bad texture entry '" + item + "' of " + s.name;
        }
        ok = false;
      }
      at = next;
    }
  });
  return ok;
}

const PackageShader* Package::Find(bool vertex, uint64_t key) const {
  const auto it = by_key_[vertex].find(key);
  return it == by_key_[vertex].end() ? nullptr : &shaders_[it->second];
}

std::vector<uint8_t> Package::ReadDksh(const PackageShader& shader) const {
  std::vector<uint8_t> data;
  FILE* f = std::fopen((dir_ + "/" + shader.name + ".dksh").c_str(), "rb");
  if (!f) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    data.resize(size_t(size));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
      data.clear();
    }
  }
  std::fclose(f);
  return data;
}

}  // namespace sdnx::dk
