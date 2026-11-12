// Keys of Xenos shader containers for the SDNX shader package (XXH3-64, as the SDK hashes guest memory):
//   raw:    the microcode bytes as stored (big-endian) - pixel shaders are looked up by it;
//   masked: the same with every vertex fetch instruction (3 words) zeroed - vertex shaders are looked up by it,
//           because Direct3D rewrites the fetches at bind (the renderer computes the same key at run time).
// Vertex fetches are found from the control flow: exec clauses, sequence bit 0 = fetch, word 0 opcode 0.
// Output per container: "<raw hex> <masked hex> <file>".
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void mask_vertex_fetches(unsigned char* m, uint32_t words) {
  uint32_t first_instr = UINT32_MAX;
  for (uint32_t i = 0;; i++) {
    if (first_instr != UINT32_MAX && i >= first_instr) break;
    if (i * 3 + 2 >= words) break;
    uint32_t a = be32(m + i * 12), b = be32(m + i * 12 + 4), c = be32(m + i * 12 + 8);
    uint64_t cfs[2] = {(uint64_t)a | ((uint64_t)(b & 0xFFFF) << 32), (uint64_t)(b >> 16) | ((uint64_t)c << 16)};
    for (int k = 0; k < 2; k++) {
      uint32_t op = (uint32_t)(cfs[k] >> 44) & 0xF;
      if (!(op >= 1 && op <= 6) && op != 13 && op != 14) continue;
      uint32_t addr = (uint32_t)cfs[k] & 0xFFF, count = (uint32_t)(cfs[k] >> 12) & 7,
               seq = (uint32_t)(cfs[k] >> 16) & 0xFFF;
      if (addr < first_instr) first_instr = addr;
      for (uint32_t j = 0; j < count; j++) {
        if (!((seq >> (2 * j)) & 1)) continue;
        uint32_t g = (addr + j) * 3;
        if (g + 2 < words && (be32(m + g * 4) & 0x1F) == 0) memset(m + g * 4, 0, 12);
      }
    }
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    FILE* f = fopen(argv[i], "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); continue; }
    fclose(f);
    uint32_t virt = be32(d + 4), hdr = be32(d + 24);
    uint32_t start = virt + be32(d + hdr), size = be32(d + hdr + 4);
    unsigned char* m = malloc(size);
    memcpy(m, d + start, size);
    if (be32(d) & 1) mask_vertex_fetches(m, size / 4);
    printf("%016llX %016llX %s\n", (unsigned long long)XXH3_64bits(d + start, size),
           (unsigned long long)XXH3_64bits(m, size), argv[i]);
    free(m);
    free(d);
  }
  return 0;
}
