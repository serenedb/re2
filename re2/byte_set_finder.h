// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_BYTE_SET_FINDER_H_
#define RE2_BYTE_SET_FINDER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace re2 {

class ByteSetFinder {
 public:
  ByteSetFinder() = default;

  bool Build(const uint64_t (&bits)[4]) {
    uint8_t row_bit[16] = {};
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

  const char* Find(const char* p, const char* end) const {
#if defined(__AVX2__)
    const __m256i lo = _mm256_broadcastsi128_si256(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(lo_)));
    const __m256i hi = _mm256_broadcastsi128_si256(
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(hi_)));
    const __m256i nibble = _mm256_set1_epi8(0x0F);
    const auto hits = [&](const char* at) {
      const __m256i bytes =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(at));
      const __m256i cols =
          _mm256_shuffle_epi8(lo, _mm256_and_si256(bytes, nibble));
      const __m256i rows = _mm256_shuffle_epi8(
          hi, _mm256_and_si256(_mm256_srli_epi16(bytes, 4), nibble));
      const __m256i miss = _mm256_cmpeq_epi8(_mm256_and_si256(cols, rows),
                                             _mm256_setzero_si256());
      return ~static_cast<uint32_t>(_mm256_movemask_epi8(miss));
    };
    if (end - p >= 32) {
      const char* tail = end - 32;
      for (; p < tail; p += 32) {
        const uint32_t mask = hits(p);
        if (mask != 0)
          return p + __builtin_ctz(mask);
      }
      const uint32_t mask =
          hits(tail) & (~uint32_t{0} << static_cast<uint32_t>(p - tail));
      return mask != 0 ? tail + __builtin_ctz(mask) : NULL;
    }
#elif defined(__ARM_NEON)
    const uint8x16_t lo = vld1q_u8(lo_);
    const uint8x16_t hi = vld1q_u8(hi_);
    const uint8x16_t nibble = vdupq_n_u8(0x0F);
    for (; end - p >= 16; p += 16) {
      const uint8x16_t bytes = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
      const uint8x16_t cols = vqtbl1q_u8(lo, vandq_u8(bytes, nibble));
      const uint8x16_t rows = vqtbl1q_u8(hi, vshrq_n_u8(bytes, 4));
      const uint8x16_t found = vtstq_u8(cols, rows);
      const uint64_t mask = vget_lane_u64(
          vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(found), 4)), 0);
      if (mask != 0)
        return p + (__builtin_ctzll(mask) >> 2);
    }
#endif
    for (; p != end; p++)
      if (Contains(bits_, *p))
        return p;
    return NULL;
  }

 private:
  static bool Contains(const uint64_t (&bits)[4], char c) {
    const uint8_t b = static_cast<uint8_t>(c);
    return ((bits[b >> 6] >> (b & 63)) & 1) != 0;
  }

  alignas(16) uint8_t lo_[16] = {};
  alignas(16) uint8_t hi_[16] = {};
  uint64_t bits_[4] = {};
};

}  // namespace re2

#endif  // RE2_BYTE_SET_FINDER_H_
