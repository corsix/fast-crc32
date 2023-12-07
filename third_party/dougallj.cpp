// https://gist.github.com/dougallj/263f132023f590aec31a11bbc746b897
// Demo for "Faster CRC32 on the Apple M1"
// https://dougallj.wordpress.com/2022/05/22/faster-crc32-on-the-apple-m1/
// I rarely use C++, but I had some fun using constexpr functions to
// compute the various constants, so you can just specify the polynomial
// as a template parameter.
//
// THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <cstring>

#include <arm_neon.h>

#ifndef NO_CRC_INSNS
#include <arm_acle.h>
#endif

// How many independent latency chains to use
inline constexpr int NUM_CHAINS = 12;
static_assert(NUM_CHAINS > 0);    // required for correctness
static_assert(NUM_CHAINS < 1000); // what is this, a gpu?

// CRC polynomial, reflected, with implicit x^32
inline constexpr uint64_t CRC32_POLY = 0xedb88320;  // CRC-32
inline constexpr uint64_t CRC32C_POLY = 0x82f63b78; // CRC-32C

template <uint32_t Poly, int NumChains>
uint32_t generic_crc32(uint32_t crc, uint8_t *p, size_t size);

uint32_t crc32(uint32_t crc, uint8_t *p, size_t size) {
  return generic_crc32<CRC32_POLY, NUM_CHAINS>(crc, p, size);
}

uint32_t crc32c(uint32_t crc, uint8_t *p, size_t size) {
  return generic_crc32<CRC32C_POLY, NUM_CHAINS>(crc, p, size);
}

template <uint32_t Poly>
constexpr uint32_t slow_crc32b(uint32_t v, uint64_t d) {
  v ^= d;
  for (int i = 0; i < 8; i++)
    v = (v >> 1) ^ (v & 1 ? Poly : 0);
  return v;
}

template <uint32_t Poly> constexpr auto build_crc32_table() {
  std::array<uint32_t, 256> arr{};
  for (int i = 0; i < 256; i++)
    arr[i] = slow_crc32b<Poly>(0, i);
  return arr;
}

template <uint32_t Poly>
inline constexpr auto crc32_table = build_crc32_table<Poly>();

template <uint32_t Poly>
__attribute__((always_inline)) static inline uint32_t crc32b(uint32_t v, uint8_t d) {
#ifndef NO_CRC_INSNS
  if constexpr (Poly == CRC32_POLY)
    return __crc32b(v, d);
  else if constexpr (Poly == CRC32C_POLY)
    return __crc32cb(v, d);
  else
#endif
    return crc32_table<Poly>[d ^ (v & 0xFF)] ^ (v >> 8);
}

template <uint32_t Poly>
__attribute__((always_inline)) static inline uint32_t crc32d(uint32_t v, uint64_t d) {
#ifndef NO_CRC_INSNS
  if constexpr (Poly == CRC32_POLY)
    return __crc32d(v, d);
  else if constexpr (Poly == CRC32C_POLY)
    return __crc32cd(v, d);
  else
#endif
  {
    for (int i = 0; i < 8; i++)
      v = crc32b<Poly>(v, (d >> (i * 8)) & 0xFF);
    return v;
  }
}

template <uint32_t Poly> inline constexpr uint32_t x_to_n_mod_p(int n) {
  uint32_t r = (uint32_t)1 << 31;
  for (int i = 0; i < n; i++) {
    r = (r >> 1) ^ (r & 1 ? Poly : 0);
  }
  return r;
}

template <uint32_t Poly> inline constexpr uint64_t x_to_n_div_p(int n) {
  uint32_t r = (uint32_t)1 << 31;
  uint64_t q = 0;
  for (int i = 0; i < n; i++) {
    q |= (uint64_t)(r & 1) << i;
    r = (r >> 1) ^ (r & 1 ? Poly : 0);
  }
  return q;
}

template <uint32_t Poly> inline constexpr uint64_t k_shift(int n) {
  return (uint64_t)x_to_n_mod_p<Poly>(n) << 1;
}

__attribute__((always_inline)) static inline uint8x16_t
reduce(uint8x16_t a, uint8x16_t b, poly64x2_t k) {
  asm("pmull  v0.1q, %[val].1d, %[consts].1d      \n\t"
      "eor    v0.16b, v0.16b, %[data].16b         \n\t"
      "pmull2 %[val].1q, %[val].2d, %[consts].2d  \n\t"
      "eor    %[val].16b, %[val].16b, v0.16b      \n\t"
      : [val] "+w"(a)
      : [consts] "w"(k), [data] "w"(b)
      : "v0");
  return a;
}

template <uint32_t Poly, int NumChains>
uint32_t generic_crc32(uint32_t crc, uint8_t *p, size_t size) {
  uint8_t *end = p + size;

  // align
  uint32_t result = ~crc;
  while (p != end && ((uintptr_t)p & 15) != 0) {
    result = crc32b<Poly>(result, *p++);
  }

  // NOTE: really not optimised for small sizes, but at least this seems
  // to make it work.
  size = (uintptr_t)end - (uintptr_t)p;
  if (end - p < 16 * NumChains) {
    while (size > 8) {
      uint64_t word;
      memcpy(&word, p, sizeof word);
      result = crc32d<Poly>(result, word);
      p += 8;
      size -= 8;
    }
    while (size > 0) {
      result = crc32b<Poly>(result, *p);
      p++;
      size--;
    }
    return ~result;
  }

  // load first 16 * NumChains chunk
  uint8x16_t vals[NumChains];
  for (int i = 0; i < NumChains; i++) {
    vals[i] = vld1q_u8(p);
    p += 0x10;
  }

  // fold in the initial crc value
  uint32x4_t init = {result, 0, 0, 0};
  vals[0] = veorq_u8(vals[0], init);

  // fold in 16 * NumChains bytes at a time
  constexpr poly64x2_t k1k2 = {k_shift<Poly>(NumChains * 128 + 32),
                               k_shift<Poly>(NumChains * 128 - 32)};

  size = (uintptr_t)end - (uintptr_t)p;
  size_t fast_size = size / (16 * NumChains) * (16 * NumChains);
  uint8_t *fast_end = p + fast_size;
  while (p != fast_end) {
#pragma unroll
    for (int i = 0; i < NumChains; i++) {
      vals[i] = reduce(vals[i], vld1q_u8(p), k1k2);
      p += 0x10;
    }
  }

  // fold down to 16-bytes
  constexpr poly64x2_t k3k4 = {k_shift<Poly>(128 + 32),
                               k_shift<Poly>(128 - 32)};

  // TODO: use a less serial reduction since this has ~72c latency
  uint64x2_t x = vals[0];
  for (int i = 1; i < NumChains; i++)
    x = reduce(x, vals[i], k3k4);

  // fold remaining 16-bytes chunks
  size = (uintptr_t)end - (uintptr_t)p;
  while (size >= 16) {
    x = reduce(x, vld1q_u8(p), k3k4);
    p += 0x10;
    size -= 0x10;
  }

  uint64x2_t message128 = x;
#ifndef NO_CRC_INSNS
  // This is simpler and faster if we can use the CRC32 instructions
  if constexpr (Poly == CRC32_POLY || Poly == CRC32C_POLY) {
    result = crc32d<Poly>(0, x[0]);
    result = crc32d<Poly>(result, x[1]);
  } else
#endif
  {
    // Barrett reduction technique
    
    // Explanation may be incorrect, just records my current best understanding.

    // CRC32(M(x)) = x^32 • M(x) mod P(x)
    //
    // Our message128 register contains M(x), so
    //
    // x^32 • M(x) =
    //
    //    [------------------------- message128 -------------------------][- zeros (32) -]
    //
    // Do two more folds to reduce this to 64-bits, so we can use the 64-bit
    // PMULL instructions to perform the Barrett reduction.

    // First fold down to 96-bit:
    //    [-------- message128[0] -------][-------- message128[1] -------][- zeros (32) -]
    //                 |                                        xor
    //                 '----- pmull ----> [-------------------- (96) --------------------]
    //                                                           =
    //                                    [------------------ message96 -----------------]

    constexpr poly64x2_t k96k64 = {k_shift<Poly>(96), k_shift<Poly>(64)};
    uint32x4_t message96 =
        veorq_u8(uint64x2_t{message128[1], 0},
                 vreinterpretq_u8_p128(vmull_p64(message128[0], k96k64[0])));

    // Then fold down to 64-bit:
    //                                    [--------------- message96 (96) ---------------]
    //                                    [ message96[0] ][ message96[1] ][ message96[2] ]
    //                                            |                      xor
    //                                            '-----> [------- (64-bit result) ------]
    //                                                                    =
    //                                                    [---------- message64 ---------]
    uint32x4_t rotated = vextq_u32(message96, message96, 1);
    message96[2] = message96[0];

    uint64x2_t message64 = veorq_u8(
        rotated, vreinterpretq_u8_p128(vmull_high_p64(message96, k96k64)));

    // message64 = x^32 • M(x), so now we just need to compute message64 mod
    // P(x).
    //
    // The Barrett reduction, for unsigned integers, using C-like (flooring)
    // division is:
    //
    //   x % N = x - (x / N) * N
    //
    // As N is constant, this can use reciprocal division instead. The same
    // general idea applies to our polynomials:

    constexpr poly64x2_t u = {x_to_n_div_p<Poly>(64) >> 31, 0};
    uint32x4_t t1 = vreinterpretq_u8_p128(vmull_p64(message64[0], u[0]));
    t1[1] = 0;

    constexpr poly64x2_t poly = {((uint64_t)Poly << 1) | 1, 0};
    uint32x4_t tmp = veorq_u8(
        vreinterpretq_u8_p128(vmull_p64(vreinterpretq_p64_u8(t1)[0], poly[0])),
        message64);
    result = tmp[1];
  }

  while (p != end)
    result = crc32b<Poly>(result, *p++);

  return ~result;
}

extern "C" {
uint32_t crc32_impl(uint32_t crc, uint8_t *p, size_t size) {
    return generic_crc32<CRC32_POLY,12>(crc, p, size);
}
}