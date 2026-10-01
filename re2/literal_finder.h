// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_LITERAL_FINDER_H_
#define RE2_LITERAL_FINDER_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "absl/log/absl_check.h"
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
      return Scan(needle, p, last);
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

  static bool Equal(const char* a, const char* b, size_t n) {
    if (n >= 8) {
      for (size_t i = 0; i + 8 < n; i += 8)
        if (!Same<uint64_t>(a + i, b + i))
          return false;
      return Same<uint64_t>(a + n - 8, b + n - 8);
    }
    if (n >= 4)
      return Same<uint32_t>(a, b) && Same<uint32_t>(a + n - 4, b + n - 4);
    if (n >= 2)
      return Same<uint16_t>(a, b) && Same<uint16_t>(a + n - 2, b + n - 2);
    return n == 0 || *a == *b;
  }

 private:
  template <typename T>
  static bool Same(const char* a, const char* b) {
    T x;
    T y;
    memcpy(&x, a, sizeof(x));
    memcpy(&y, b, sizeof(y));
    return x == y;
  }

#if defined(__clang__)
  const char* Scan(absl::string_view needle, const char* p,
                   const char* last) const {
    const char first = needle[first_];
    const char second = needle[second_];
    const char* tail = last - 31;
    [[clang::code_align(64)]] for (; p < tail; p += 32) {
      uint32_t mask = Hits(p, first, second);
      if (__builtin_expect(mask != 0, 0)) {
        for (; mask != 0; mask &= mask - 1) {
          const char* at = p + __builtin_ctz(mask);
          if (Equal(at, needle.data(), needle.size()))
            return at;
        }
      }
    }
    uint32_t mask = Hits(tail, first, second) &
                    (~uint32_t{0} << static_cast<uint32_t>(p - tail));
    for (; mask != 0; mask &= mask - 1) {
      const char* at = tail + __builtin_ctz(mask);
      if (Equal(at, needle.data(), needle.size()))
        return at;
    }
    return NULL;
  }

  uint32_t Hits(const char* p, char first, char second) const {
    using Block = char __attribute__((vector_size(32)));
    using Bits = bool __attribute__((ext_vector_type(32)));
    Block a;
    Block b;
    memcpy(&a, p + first_, sizeof(a));
    memcpy(&b, p + second_, sizeof(b));
    const Bits hits = __builtin_convertvector((a == first) & (b == second),
                                              Bits);
    uint32_t mask;
    memcpy(&mask, &hits, sizeof(mask));
    return mask;
  }
#endif

  static int Rank(char c) {
    return literal_finder_internal::kRanks.rank[static_cast<uint8_t>(c)];
  }

  size_t size_ = 0;
  size_t first_ = 0;
  size_t second_ = 1;
};

}  // namespace re2

#endif  // RE2_LITERAL_FINDER_H_
