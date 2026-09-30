// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_SEGMENT_PLAN_H_
#define RE2_SEGMENT_PLAN_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "re2/literal_finder.h"

namespace re2 {

class Regexp;

class SegmentPlan {
 public:
  static SegmentPlan* Make(Regexp* re);

  bool Match(absl::string_view text) const;

 private:
  struct Piece {
    uint32_t skip;
    uint32_t offset;
    uint32_t size;
  };

  struct Segment {
    uint32_t begin;
    uint32_t end;
    LiteralFinder finder;
  };

  SegmentPlan() = default;

  size_t UnitAt(const char* p, const char* end) const;
  size_t UnitBefore(const char* begin, const char* p) const;
  bool Units(absl::string_view text) const;
  size_t MatchAt(const Piece* piece, const Piece* end, absl::string_view text,
                 size_t pos, size_t limit) const;
  size_t MatchBefore(const Segment& segment, absl::string_view text,
                     size_t lower) const;
  size_t FindFrom(const Segment& segment, absl::string_view text, size_t pos,
                  size_t limit) const;

  std::string bytes_;
  std::vector<Piece> pieces_;
  std::vector<Segment> segments_;
  bool any_string_ = false;
  bool latin1_ = false;
  bool dot_nl_ = true;
};

}  // namespace re2

#endif  // RE2_SEGMENT_PLAN_H_
