// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_BYTE_SET_FINDER_H_
#define RE2_BYTE_SET_FINDER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "absl/numeric/bits.h"

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSSE3__)
#include <tmmintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace re2 {

class ByteSetFinder {
 public:
  ByteSetFinder() = default;

  bool Build(const uint64_t* bits) {
    uint8_t row_bit[16] = {};
    memset(lo_, 0, sizeof(lo_));
    int rows = 0;
    for (int b = 0; b < 256; b++) {
      if (!Contains(bits, static_cast<uint8_t>(b)))
        continue;
      const int row = b >> 4;
      if (row_bit[row] == 0) {
        if (rows == 8)
          return false;
        row_bit[row] = static_cast<uint8_t>(1 << rows++);
      }
      lo_[b & 15] |= row_bit[row];
    }
    memcpy(hi_, row_bit, sizeof(hi_));
    memcpy(bits_, bits, sizeof(bits_));
    return rows != 0;
  }

  uint32_t Classify32(const char* block) const {
#if defined(__AVX2__)
    const __m256i lo = _mm256_broadcastsi128_si256(
        _mm_load_si128(reinterpret_cast<const __m128i*>(lo_)));
    const __m256i hi = _mm256_broadcastsi128_si256(
        _mm_load_si128(reinterpret_cast<const __m128i*>(hi_)));
    const __m256i nibble = _mm256_set1_epi8(0x0F);
    const __m256i bytes =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block));
    const __m256i cols =
        _mm256_shuffle_epi8(lo, _mm256_and_si256(bytes, nibble));
    const __m256i rows = _mm256_shuffle_epi8(
        hi, _mm256_and_si256(_mm256_srli_epi16(bytes, 4), nibble));
    const __m256i miss = _mm256_cmpeq_epi8(_mm256_and_si256(cols, rows),
                                           _mm256_setzero_si256());
    return ~static_cast<uint32_t>(_mm256_movemask_epi8(miss));
#elif defined(__SSSE3__)
    const __m128i lo = _mm_load_si128(reinterpret_cast<const __m128i*>(lo_));
    const __m128i hi = _mm_load_si128(reinterpret_cast<const __m128i*>(hi_));
    const __m128i nibble = _mm_set1_epi8(0x0F);
    uint32_t mask = 0;
    for (int half = 0; half < 32; half += 16) {
      const __m128i bytes =
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(block + half));
      const __m128i cols = _mm_shuffle_epi8(lo, _mm_and_si128(bytes, nibble));
      const __m128i rows = _mm_shuffle_epi8(
          hi, _mm_and_si128(_mm_srli_epi16(bytes, 4), nibble));
      const __m128i miss =
          _mm_cmpeq_epi8(_mm_and_si128(cols, rows), _mm_setzero_si128());
      mask |= (~static_cast<uint32_t>(_mm_movemask_epi8(miss)) & 0xFFFF)
              << half;
    }
    return mask;
#elif defined(__aarch64__) && defined(__ARM_NEON)
    static const uint8_t kBits[16] = {1, 2, 4, 8, 16, 32, 64, 128,
                                      1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t lo = vld1q_u8(lo_);
    const uint8x16_t hi = vld1q_u8(hi_);
    const uint8x16_t nibble = vdupq_n_u8(0x0F);
    const uint8x16_t bits = vld1q_u8(kBits);
    const auto found = [&](const char* at) {
      const uint8x16_t bytes = vld1q_u8(reinterpret_cast<const uint8_t*>(at));
      return vandq_u8(vtstq_u8(vqtbl1q_u8(lo, vandq_u8(bytes, nibble)),
                               vqtbl1q_u8(hi, vshrq_n_u8(bytes, 4))),
                      bits);
    };
    uint8x16_t sum = vpaddq_u8(found(block), found(block + 16));
    sum = vpaddq_u8(sum, sum);
    sum = vpaddq_u8(sum, sum);
    return vgetq_lane_u32(vreinterpretq_u32_u8(sum), 0);
#else
    uint32_t mask = 0;
    for (int i = 0; i < 32; i++)
      mask |= static_cast<uint32_t>(Contains(bits_, block[i])) << i;
    return mask;
#endif
  }

  const char* Find(const char* p, const char* end) const {
    if (end - p >= 32) {
      const char* tail = end - 32;
      for (; p < tail; p += 32) {
        const uint32_t mask = Classify32(p);
        if (mask != 0)
          return p + absl::countr_zero(mask);
      }
      const uint32_t mask =
          Classify32(tail) & (~uint32_t{0} << static_cast<uint32_t>(p - tail));
      return mask != 0 ? tail + absl::countr_zero(mask) : NULL;
    }
    for (; p != end; p++)
      if (Contains(bits_, *p))
        return p;
    return NULL;
  }

 private:
  static bool Contains(const uint64_t* bits, char c) {
    const uint8_t b = static_cast<uint8_t>(c);
    return ((bits[b >> 6] >> (b & 63)) & 1) != 0;
  }

  alignas(16) uint8_t lo_[16] = {};
  alignas(16) uint8_t hi_[16] = {};
  uint64_t bits_[4] = {};
};

}  // namespace re2

#endif  // RE2_BYTE_SET_FINDER_H_
