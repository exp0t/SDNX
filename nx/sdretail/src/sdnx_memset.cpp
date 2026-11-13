// Sleeping Dogs for Nintendo Switch: memset for zero fills of 160 bytes and more (linked with -Wl,--wrap=memset).
//
// newlib's AArch64 memset takes, for those, a DC ZVA path that first reads DCZID_EL0. On the console one instruction
// right after that read held 10.6 % of the GPU command thread's samples (profile 2026-11-13): the native renderer
// zeroes a 424-byte pipeline key and a 944-byte constant block on every draw. Here the fill is plain 64-byte stores of
// a zeroed vector register; everything else goes to newlib's memset unchanged.

#if defined(__SWITCH__) && defined(__aarch64__)

#include <cstddef>
#include <cstdint>

extern "C" void* __real_memset(void* dst, int c, size_t n);

extern "C" void* __wrap_memset(void* dst, int c, size_t n) {
  if ((c & 0xFF) != 0 || n < 160) return __real_memset(dst, c, n);
  uint8_t* p = static_cast<uint8_t*>(dst);
  uint8_t* end = p + n;
  // Unaligned 64-byte head, 64-byte aligned body, unaligned 64-byte tail (the stores may overlap). Written in assembly
  // so the compiler cannot turn the loop back into a memset call.
  __asm__ volatile(
      "movi v0.16b, #0\n"
      "stp q0, q0, [%[p]]\n"
      "stp q0, q0, [%[p], #32]\n"
      "add x9, %[p], #64\n"
      "and x9, x9, #-64\n"
      "sub x10, %[e], #64\n"
      "1:\n"
      "cmp x9, x10\n"
      "b.hs 2f\n"
      "stp q0, q0, [x9]\n"
      "stp q0, q0, [x9, #32]\n"
      "add x9, x9, #64\n"
      "b 1b\n"
      "2:\n"
      "stp q0, q0, [x10]\n"
      "stp q0, q0, [x10, #32]\n"
      :
      : [p] "r"(p), [e] "r"(end)
      : "x9", "x10", "v0", "memory", "cc");
  return dst;
}

#endif
