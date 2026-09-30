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

class LiteralFinder {
 public:
  LiteralFinder() = default;

  explicit LiteralFinder(absl::string_view needle) : size_(needle.size()) {
    if (size_ < 2)
      return;
    size_t best = 0;
    for (size_t i = 1; i < size_; i++)
      if (Rank(needle[i]) < Rank(needle[best]))
        best = i;
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
    const char first = needle[first_];
    const char second = needle[second_];
#if defined(__clang__)
    using Block = char __attribute__((vector_size(32)));
    using Bits = bool __attribute__((ext_vector_type(32)));
    for (; last - p >= 31; p += 32) {
      Block a;
      Block b;
      memcpy(&a, p + first_, sizeof(a));
      memcpy(&b, p + second_, sizeof(b));
      const Bits hits = __builtin_convertvector((a == first) & (b == second),
                                                Bits);
      uint32_t mask;
      memcpy(&mask, &hits, sizeof(mask));
      while (mask != 0) {
        const char* at = p + __builtin_ctz(mask);
        if (memcmp(at, needle.data(), n) == 0)
          return at;
        mask &= mask - 1;
      }
    }
#endif
    for (; p <= last; p++) {
      if (p[first_] == first && p[second_] == second &&
          memcmp(p, needle.data(), n) == 0)
        return p;
    }
    return NULL;
  }

 private:
  static int Rank(char c) {
    const uint8_t b = static_cast<uint8_t>(c);
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

  static constexpr uint8_t kLetterRank[26] = {
      8, 5, 6, 6, 8, 6, 6, 8, 8, 3, 5, 6, 6,
      8, 8, 6, 3, 8, 8, 8, 6, 5, 6, 3, 6, 3,
  };

  size_t size_ = 0;
  size_t first_ = 0;
  size_t second_ = 1;
};

}  // namespace re2

#endif  // RE2_LITERAL_FINDER_H_
