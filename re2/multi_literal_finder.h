// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_MULTI_LITERAL_FINDER_H_
#define RE2_MULTI_LITERAL_FINDER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <numeric>
#include <string>

#include "absl/numeric/bits.h"
#include "absl/strings/string_view.h"
#include "re2/literal_finder.h"

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSSE3__)
#include <tmmintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace re2 {

template <size_t kCapacity>
class BasicMultiLiteralFinder {
 public:
  static constexpr size_t kMaxLiterals = kCapacity;

  BasicMultiLiteralFinder() = default;

  template <typename LiteralAt>
  bool Build(size_t count, LiteralAt literal_at) {
    if (count == 0 || count > kMaxLiterals)
      return false;
    const size_t n = count;
    const char* data[kMaxLiterals];
    size_t sizes[kMaxLiterals];
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
      const absl::string_view s = literal_at(i);
      data[i] = s.data();
      sizes[i] = s.size();
      total += s.size();
    }
    min_size_ = *std::min_element(sizes, sizes + n);
    if (min_size_ == 0)
      return false;
    const size_t window = std::min(min_size_, kMaxOffset + 1);
    int score[kMaxOffset + 1] = {};
    for (size_t i = 0; i < n; i++)
      for (size_t o = 0; o < window; o++)
        score[o] += Rank(data[i][o]);
    uint8_t order[kMaxOffset + 1];
    std::iota(order, order + window, 0);
    std::stable_sort(order, order + window,
                     [&](uint8_t a, uint8_t b) { return score[a] < score[b]; });
    noffsets_ = static_cast<int>(std::min<size_t>(kMaxOffsets, window));
    std::copy(order, order + noffsets_, offsets_);
    std::sort(offsets_, offsets_ + noffsets_);
    max_offset_ = offsets_[noffsets_ - 1];

    uint8_t idx[kMaxLiterals];
    std::iota(idx, idx + n, 0);
    if (n > kBuckets) {
      uint32_t keys[kMaxLiterals];
      for (size_t i = 0; i < n; i++) {
        uint32_t key = 0;
        for (int m = 0; m < noffsets_; m++)
          key = key << 8 | static_cast<uint8_t>(data[i][offsets_[m]]);
        keys[i] = key;
      }
      std::sort(idx, idx + n, [&](uint8_t a, uint8_t b) {
        return keys[a] != keys[b] ? keys[a] < keys[b] : a < b;
      });
    }

    memset(lo_, 0, sizeof(lo_));
    memset(hi_, 0, sizeof(hi_));
    bytes_.resize(total);
    starts_[0] = 0;
    for (int b = 0; b <= kBuckets; b++)
      bucket_begin_[b] = static_cast<uint8_t>(n * b / kBuckets);
    for (int b = 0; b < kBuckets; b++) {
      for (size_t i = bucket_begin_[b]; i < bucket_begin_[b + 1]; i++) {
        const char* s = data[idx[i]];
        for (int m = 0; m < noffsets_; m++) {
          const uint8_t c = static_cast<uint8_t>(s[offsets_[m]]);
          lo_[m][c & 15] |= static_cast<uint8_t>(1 << b);
          hi_[m][c >> 4] |= static_cast<uint8_t>(1 << b);
        }
        memcpy(&bytes_[starts_[i]], s, sizes[idx[i]]);
        starts_[i + 1] = static_cast<uint32_t>(starts_[i] + sizes[idx[i]]);
      }
    }
    return true;
  }

  size_t min_size() const { return min_size_; }

  const char* Find(const char* p, const char* end) const {
    if (noffsets_ == 1)
      return Scan<1>(p, end);
    if (noffsets_ == 2)
      return Scan<2>(p, end);
    return Scan<3>(p, end);
  }

  template <typename OnCandidate>
  void ForEachCandidate(const char* p, const char* end,
                        OnCandidate&& on_candidate) const {
    if (noffsets_ == 1)
      return Candidates<1>(p, end, on_candidate);
    if (noffsets_ == 2)
      return Candidates<2>(p, end, on_candidate);
    return Candidates<3>(p, end, on_candidate);
  }

 private:
  static constexpr int kMaxOffsets = 3;
  static constexpr int kBuckets = 8;
  static constexpr size_t kMaxOffset = 15;

  static int Rank(char c) {
    return literal_finder_internal::kRanks.rank[static_cast<uint8_t>(c)];
  }

#if defined(__AVX2__)
  using Vec = __m256i;
#elif defined(__SSSE3__)
  using Vec = __m128i;
#elif defined(__aarch64__) && defined(__ARM_NEON)
  using Vec = uint8x16_t;
#else
  using Vec = uint8_t;
#endif

  struct Masks {
    Vec lo[kMaxOffsets];
    Vec hi[kMaxOffsets];
  };

  template <int M>
  Masks Load() const {
    Masks masks;
    for (int m = 0; m < M; m++) {
#if defined(__AVX2__)
      masks.lo[m] = _mm256_broadcastsi128_si256(
          _mm_load_si128(reinterpret_cast<const __m128i*>(lo_[m])));
      masks.hi[m] = _mm256_broadcastsi128_si256(
          _mm_load_si128(reinterpret_cast<const __m128i*>(hi_[m])));
#elif defined(__SSSE3__)
      masks.lo[m] = _mm_load_si128(reinterpret_cast<const __m128i*>(lo_[m]));
      masks.hi[m] = _mm_load_si128(reinterpret_cast<const __m128i*>(hi_[m]));
#elif defined(__aarch64__) && defined(__ARM_NEON)
      masks.lo[m] = vld1q_u8(lo_[m]);
      masks.hi[m] = vld1q_u8(hi_[m]);
#else
      masks.lo[m] = 0;
      masks.hi[m] = 0;
#endif
    }
    return masks;
  }

  template <int M>
  const char* Scan(const char* p, const char* end) const {
    alignas(32) uint8_t lanes[32];
    if (static_cast<size_t>(end - p) >= max_offset_ + 32) {
      const Masks masks = Load<M>();
      const char* last = end - max_offset_ - 32;
      while (p < last) {
        uint32_t mask = 0;
        for (; p < last; p += 32) {
          mask = Hits<M>(masks, p, lanes);
          if (mask != 0)
            break;
        }
        if (mask == 0)
          break;
        const char* at = Verify(p, end, mask, lanes);
        if (at != NULL)
          return at;
        p += 32;
      }
      const uint32_t mask = Hits<M>(masks, last, lanes) &
                            (~uint32_t{0} << static_cast<uint32_t>(p - last));
      return mask != 0 ? Verify(last, end, mask, lanes) : NULL;
    }
    for (; static_cast<size_t>(end - p) >= min_size_; p++) {
      uint32_t bits = 0xFF;
      for (int m = 0; m < M; m++) {
        const uint8_t c = static_cast<uint8_t>(p[offsets_[m]]);
        bits &= lo_[m][c & 15] & hi_[m][c >> 4];
      }
      if (bits != 0) {
        lanes[0] = static_cast<uint8_t>(bits);
        const char* at = Verify(p, end, 1, lanes);
        if (at != NULL)
          return at;
      }
    }
    return NULL;
  }

  template <int M, typename OnCandidate>
  void Candidates(const char* p, const char* end,
                  OnCandidate& on_candidate) const {
    if (static_cast<size_t>(end - p) >= max_offset_ + 32) {
      alignas(32) uint8_t lanes[32];
      const Masks masks = Load<M>();
      const char* last = end - max_offset_ - 32;
      const char* next = p;
      for (const char* q = p;;) {
        const char* block = q < last ? q : last;
        const size_t seen = static_cast<size_t>(q - block);
        uint32_t mask =
            seen < 32 ? Hits<M>(masks, block, lanes) & (~uint32_t{0} << seen)
                      : 0;
        while (mask != 0) {
          next = on_candidate(block + absl::countr_zero(mask));
          const size_t skip = static_cast<size_t>(next - block);
          mask = skip < 32 ? mask & (~uint32_t{0} << skip) : 0;
        }
        if (block == last)
          return;
        q = std::max(block + 32, next);
      }
    }
    while (static_cast<size_t>(end - p) >= min_size_) {
      uint32_t bits = 0xFF;
      for (int m = 0; m < M; m++) {
        const uint8_t c = static_cast<uint8_t>(p[offsets_[m]]);
        bits &= lo_[m][c & 15] & hi_[m][c >> 4];
      }
      p = bits != 0 ? on_candidate(p) : p + 1;
    }
  }

  const char* Verify(const char* q, const char* end, uint32_t mask,
                     const uint8_t* lanes) const {
    for (; mask != 0; mask &= mask - 1) {
      const int j = absl::countr_zero(mask);
      const char* at = q + j;
      const size_t left = static_cast<size_t>(end - at);
      for (uint32_t bits = lanes[j]; bits != 0; bits &= bits - 1) {
        const int b = absl::countr_zero(bits);
        for (int i = bucket_begin_[b]; i < bucket_begin_[b + 1]; i++) {
          const size_t size = starts_[i + 1] - starts_[i];
          if (size <= left &&
              LiteralFinder::Equal(at, bytes_.data() + starts_[i], size))
            return at;
        }
      }
    }
    return NULL;
  }

  template <int M>
  uint32_t Hits(const Masks& masks, const char* q, uint8_t* lanes) const {
#if defined(__AVX2__)
    const __m256i nibble = _mm256_set1_epi8(0x0F);
    __m256i acc = _mm256_set1_epi8(-1);
    for (int m = 0; m < M; m++) {
      const __m256i bytes =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q + offsets_[m]));
      acc = _mm256_and_si256(
          acc,
          _mm256_and_si256(
              _mm256_shuffle_epi8(masks.lo[m],
                                  _mm256_and_si256(bytes, nibble)),
              _mm256_shuffle_epi8(
                  masks.hi[m],
                  _mm256_and_si256(_mm256_srli_epi16(bytes, 4), nibble))));
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), acc);
    return ~static_cast<uint32_t>(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(acc, _mm256_setzero_si256())));
#elif defined(__SSSE3__)
    const __m128i nibble = _mm_set1_epi8(0x0F);
    uint32_t mask = 0;
    for (int half = 0; half < 32; half += 16) {
      __m128i acc = _mm_set1_epi8(-1);
      for (int m = 0; m < M; m++) {
        const __m128i bytes = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(q + half + offsets_[m]));
        acc = _mm_and_si128(
            acc,
            _mm_and_si128(
                _mm_shuffle_epi8(masks.lo[m], _mm_and_si128(bytes, nibble)),
                _mm_shuffle_epi8(
                    masks.hi[m],
                    _mm_and_si128(_mm_srli_epi16(bytes, 4), nibble))));
      }
      _mm_store_si128(reinterpret_cast<__m128i*>(lanes + half), acc);
      mask |= (~static_cast<uint32_t>(_mm_movemask_epi8(
                   _mm_cmpeq_epi8(acc, _mm_setzero_si128()))) &
               0xFFFF)
              << half;
    }
    return mask;
#elif defined(__aarch64__) && defined(__ARM_NEON)
    static const uint8_t kBits[16] = {1, 2, 4, 8, 16, 32, 64, 128,
                                      1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t nibble = vdupq_n_u8(0x0F);
    const uint8x16_t bits = vld1q_u8(kBits);
    uint8x16_t found[2];
    for (int half = 0; half < 2; half++) {
      uint8x16_t acc = vdupq_n_u8(0xFF);
      for (int m = 0; m < M; m++) {
        const uint8x16_t bytes = vld1q_u8(
            reinterpret_cast<const uint8_t*>(q + 16 * half + offsets_[m]));
        acc = vandq_u8(
            acc, vandq_u8(vqtbl1q_u8(masks.lo[m], vandq_u8(bytes, nibble)),
                          vqtbl1q_u8(masks.hi[m], vshrq_n_u8(bytes, 4))));
      }
      vst1q_u8(lanes + 16 * half, acc);
      found[half] = vandq_u8(vtstq_u8(acc, acc), bits);
    }
    uint8x16_t sum = vpaddq_u8(found[0], found[1]);
    sum = vpaddq_u8(sum, sum);
    sum = vpaddq_u8(sum, sum);
    return vgetq_lane_u32(vreinterpretq_u32_u8(sum), 0);
#else
    (void)masks;
    uint32_t mask = 0;
    for (int j = 0; j < 32; j++) {
      uint32_t acc = 0xFF;
      for (int m = 0; m < M; m++) {
        const uint8_t c = static_cast<uint8_t>(q[j + offsets_[m]]);
        acc &= lo_[m][c & 15] & hi_[m][c >> 4];
      }
      lanes[j] = static_cast<uint8_t>(acc);
      mask |= static_cast<uint32_t>(acc != 0) << j;
    }
    return mask;
#endif
  }

  alignas(16) uint8_t lo_[kMaxOffsets][16] = {};
  alignas(16) uint8_t hi_[kMaxOffsets][16] = {};
  uint8_t offsets_[kMaxOffsets] = {};
  int noffsets_ = 0;
  size_t max_offset_ = 0;
  size_t min_size_ = 0;
  uint8_t bucket_begin_[kBuckets + 1] = {};
  uint32_t starts_[kMaxLiterals + 1] = {};
  std::string bytes_;
};

using MultiLiteralFinder = BasicMultiLiteralFinder<64>;

}  // namespace re2

#endif  // RE2_MULTI_LITERAL_FINDER_H_
