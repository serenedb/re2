// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_LITERAL_FINDER_H_
#define RE2_LITERAL_FINDER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <utility>

#include "absl/log/absl_check.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"

namespace re2 {

namespace literal_finder_internal {

constexpr uint8_t kLetterRank[26] = {
    8, 5, 6, 6, 8, 6, 6, 8, 8, 3, 5, 6, 6,
    8, 8, 6, 3, 8, 8, 8, 6, 5, 6, 3, 6, 3,
};

constexpr uint8_t RankOf(uint8_t b) {
  if (b >= 0x80)
    return b < 0xC0 || b >= 0xF5 ? 1 : 9;
  if (b < 0x20)
    return b == '\n' || b == '\t' || b == '\r' ? 6 : 1;
  if (b >= 'a' && b <= 'z')
    return kLetterRank[b - 'a'];
  if (b == ' ')
    return 10;
  if (b == ',' || b == '.')
    return 6;
  return 4;
}

struct RankTable {
  constexpr RankTable() : rank() {
    for (int b = 0; b < 256; b++)
      rank[b] = RankOf(static_cast<uint8_t>(b));
  }
  uint8_t rank[256];
};

inline constexpr RankTable kRanks;

}  // namespace literal_finder_internal

class LiteralFinder {
 public:
  LiteralFinder() = default;

  explicit LiteralFinder(absl::string_view needle) : size_(needle.size()) {
    if (size_ < 2)
      return;
    size_t best = 0;
    int best_rank = Rank(needle[0]);
    for (size_t i = 1; i < size_; i++) {
      const int rank = Rank(needle[i]);
      if (rank < best_rank) {
        best = i;
        best_rank = rank;
      }
    }
    size_t other = best == 0 ? 1 : 0;
    for (size_t i = 0; i < size_; i++) {
      if (i == best)
        continue;
      const bool distinct = needle[i] != needle[best];
      const bool other_distinct = needle[other] != needle[best];
      if (distinct != other_distinct) {
        if (distinct)
          other = i;
        continue;
      }
      if (Rank(needle[i]) < Rank(needle[other]))
        other = i;
    }
    first_ = best < other ? best : other;
    second_ = best < other ? other : best;
  }

  const char* Find(absl::string_view needle, const char* p,
                   const char* end) const {
    ABSL_DCHECK_EQ(needle.size(), size_);
    const size_t n = size_;
    if (static_cast<size_t>(end - p) < n)
      return NULL;
    if (n == 0)
      return p;
    if (n == 1)
      return static_cast<const char*>(memchr(p, needle[0], end - p));
    const char* last = end - n;
#if defined(__clang__)
    if (last - p >= 31)
      return Scan<false, 32>(needle, p, last);
#endif
    const char first = needle[first_];
    const char second = needle[second_];
    while (p <= last) {
      const char* hit = static_cast<const char*>(
          memchr(p + first_, first, static_cast<size_t>(last - p) + 1));
      if (hit == NULL)
        return NULL;
      p = hit - first_;
      if (p[second_] == second && Equal(p, needle.data(), n))
        return p;
      p++;
    }
    return NULL;
  }

  const char* FindIgnoreCase(absl::string_view needle, const char* p,
                             const char* end) const {
    ABSL_DCHECK_EQ(needle.size(), size_);
    ABSL_DCHECK(std::none_of(needle.begin(), needle.end(),
                             [](char c) { return absl::ascii_isupper(c); }));
    const size_t n = size_;
    if (static_cast<size_t>(end - p) < n)
      return NULL;
    if (n == 0)
      return p;
    const char* last = end - n;
#if defined(__clang__)
    if (last - p >= 31)
      return Scan<true, 32>(needle, p, last);
    if (last - p >= 7) {
      if (last - p >= 15)
        return Scan<true, 16>(needle, p, last);
      return Scan<true, 8>(needle, p, last);
    }
#endif
    size_t rare = first_;
    size_t other = second_;
    if (Rank(needle[other]) < Rank(needle[rare]))
      std::swap(rare, other);
    const char rare_byte = needle[rare];
    const char rare_case = CaseBit(rare_byte);
    const char other_byte = needle[other];
    const char other_case = CaseBit(other_byte);
    if (rare_case == 0) {
      while (p <= last) {
        const char* hit = static_cast<const char*>(
            memchr(p + rare, rare_byte, static_cast<size_t>(last - p) + 1));
        if (hit == NULL)
          return NULL;
        p = hit - rare;
        if ((p[other] | other_case) == other_byte &&
            EqualIgnoreCase(p, needle.data(), n))
          return p;
        p++;
      }
      return NULL;
    }
    for (; p <= last; p++)
      if ((p[rare] | rare_case) == rare_byte &&
          (p[other] | other_case) == other_byte &&
          EqualIgnoreCase(p, needle.data(), n))
        return p;
    return NULL;
  }

  static bool Equal(const char* a, const char* b, size_t n) {
    return Compare<false>(a, b, n);
  }

  static bool EqualIgnoreCase(const char* text, const char* needle, size_t n) {
    return Compare<true>(text, needle, n);
  }

 private:
  template <bool kIgnoreCase>
  static bool Compare(const char* a, const char* b, size_t n) {
    if (n >= 8) {
      for (size_t i = 0; i + 8 < n; i += 8)
        if (!Same<uint64_t, kIgnoreCase>(a + i, b + i))
          return false;
      return Same<uint64_t, kIgnoreCase>(a + n - 8, b + n - 8);
    }
    if (n >= 4)
      return Same<uint32_t, kIgnoreCase>(a, b) &&
             Same<uint32_t, kIgnoreCase>(a + n - 4, b + n - 4);
    if (n >= 2)
      return Same<uint16_t, kIgnoreCase>(a, b) &&
             Same<uint16_t, kIgnoreCase>(a + n - 2, b + n - 2);
    return n == 0 || Same<uint8_t, kIgnoreCase>(a, b);
  }

  template <typename T, bool kIgnoreCase>
  static bool Same(const char* a, const char* b) {
    T x;
    T y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return (kIgnoreCase ? LowerAscii(x) : x) == y;
  }

  template <typename T>
  static T LowerAscii(T x) {
    constexpr T kBytes = static_cast<T>(~T{0}) / 0xFF;
    const T low = static_cast<T>(x & kBytes * 0x7F);
    const T upper = static_cast<T>((low + kBytes * 0x3F) &
                                   ~(low + kBytes * 0x25) & ~x &
                                   kBytes * 0x80);
    return static_cast<T>(x | upper >> 2);
  }

  static char CaseBit(char c) { return absl::ascii_islower(c) ? 0x20 : 0; }

#if defined(__clang__)
  template <bool kIgnoreCase, int kWidth>
  const char* Scan(absl::string_view needle, const char* p,
                   const char* last) const {
    const char first = needle[first_];
    const char second = needle[second_];
    const char first_case = kIgnoreCase ? CaseBit(first) : 0;
    const char second_case = kIgnoreCase ? CaseBit(second) : 0;
    const char* tail = last - (kWidth - 1);
    for (; p < tail; p += kWidth) {
      uint32_t mask = Hits<kWidth>(p, first, first_case, second, second_case);
      if (__builtin_expect(mask != 0, 0)) {
        for (; mask != 0; mask &= mask - 1) {
          const char* at = p + __builtin_ctz(mask);
          if (Compare<kIgnoreCase>(at, needle.data(), needle.size()))
            return at;
        }
      }
    }
    uint32_t mask = Hits<kWidth>(tail, first, first_case, second, second_case) &
                    (~uint32_t{0} << static_cast<uint32_t>(p - tail));
    for (; mask != 0; mask &= mask - 1) {
      const char* at = tail + __builtin_ctz(mask);
      if (Compare<kIgnoreCase>(at, needle.data(), needle.size()))
        return at;
    }
    return NULL;
  }

  template <int kWidth>
  uint32_t Hits(const char* p, char first, char first_case, char second,
                char second_case) const {
    using Block = char __attribute__((vector_size(kWidth)));
    using Bits = bool __attribute__((ext_vector_type(kWidth)));
    Block a;
    Block b;
    memcpy(&a, p + first_, sizeof(a));
    memcpy(&b, p + second_, sizeof(b));
    const Bits hits = __builtin_convertvector(
        ((a | first_case) == first) & ((b | second_case) == second), Bits);
    uint32_t mask = 0;
    memcpy(&mask, &hits, sizeof(hits));
    return mask;
  }
#endif

  static int Rank(char c) {
    return literal_finder_internal::kRanks.rank[static_cast<uint8_t>(c)];
  }

  size_t size_ = 0;
  size_t first_ = 0;
  size_t second_ = 0;
};

}  // namespace re2

#endif  // RE2_LITERAL_FINDER_H_
