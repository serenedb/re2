// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/segment_plan.h"

#include <string.h>

#include <algorithm>
#include <utility>

#include "re2/regexp.h"
#include "util/utf.h"

namespace re2 {

namespace {

constexpr size_t kNoMatch = static_cast<size_t>(-1);

size_t UnitSize(uint8_t b) {
  if (b < 0x80)
    return 1;
  if (b < 0xC2)
    return 0;
  if (b < 0xE0)
    return 2;
  if (b < 0xF0)
    return 3;
  if (b < 0xF5)
    return 4;
  return 0;
}

bool IsContinuation(uint8_t b) {
  return (b & 0xC0) == 0x80;
}

bool IsAnyExceptNewline(Regexp* re) {
  if (re->op() != kRegexpCharClass)
    return false;
  CharClass* cc = re->cc();
  const Rune max = (re->parse_flags() & Regexp::Latin1) ? 0xFF : Runemax;
  CharClass::iterator it = cc->begin();
  if (it == cc->end() || it->lo != 0 || it->hi != '\n' - 1)
    return false;
  ++it;
  if (it == cc->end() || it->lo != '\n' + 1 || it->hi != max)
    return false;
  ++it;
  return it == cc->end();
}

}  // namespace

SegmentPlan* SegmentPlan::Make(Regexp* re) {
  if (re->parse_flags() & Regexp::NeverNL)
    return NULL;
  Regexp** subs = &re;
  int nsub = 1;
  if (re->op() == kRegexpConcat) {
    subs = re->sub();
    nsub = re->nsub();
  }
  int first = 0;
  int last = nsub;
  if (first < last && subs[first]->op() == kRegexpBeginText)
    first++;
  if (first < last && subs[last - 1]->op() == kRegexpEndText)
    last--;

  SegmentPlan plan;
  plan.latin1_ = (re->parse_flags() & Regexp::Latin1) != 0;
  int dot = -1;
  bool newline_literal = false;
  Piece piece{0, 0, 0};
  uint32_t begin = 0;
  auto close_piece = [&]() {
    if (piece.skip != 0 || piece.size != 0)
      plan.pieces_.push_back(piece);
    piece = {0, static_cast<uint32_t>(plan.bytes_.size()), 0};
  };
  auto close_segment = [&]() {
    close_piece();
    const uint32_t end = static_cast<uint32_t>(plan.pieces_.size());
    plan.segments_.push_back({begin, end, LiteralFinder()});
    begin = end;
  };
  auto unit_kind = [&](Regexp* sub) {
    if (sub->op() == kRegexpAnyChar)
      return 1;
    if (IsAnyExceptNewline(sub))
      return 0;
    return -1;
  };
  auto append = [&](Rune r) {
    if (r == '\n')
      newline_literal = true;
    if (plan.latin1_) {
      plan.bytes_ += static_cast<char>(r);
      piece.size++;
      return;
    }
    char buf[UTFmax];
    const int n = runetochar(buf, &r);
    plan.bytes_.append(buf, n);
    piece.size += n;
  };
  bool fits = true;
  for (int i = first; i < last && fits; i++) {
    Regexp* sub = subs[i];
    switch (sub->op()) {
      case kRegexpLiteral:
        if (sub->parse_flags() & Regexp::FoldCase) {
          fits = false;
          break;
        }
        append(sub->rune());
        break;
      case kRegexpLiteralString:
        if (sub->parse_flags() & Regexp::FoldCase) {
          fits = false;
          break;
        }
        for (int j = 0; j < sub->nrunes(); j++)
          append(sub->runes()[j]);
        break;
      case kRegexpStar: {
        const int kind = unit_kind(sub->sub()[0]);
        if (kind < 0 || (dot >= 0 && kind != dot)) {
          fits = false;
          break;
        }
        dot = kind;
        close_segment();
        plan.any_string_ = true;
        break;
      }
      default: {
        const int kind = unit_kind(sub);
        if (kind < 0 || (dot >= 0 && kind != dot)) {
          fits = false;
          break;
        }
        dot = kind;
        if (piece.size != 0)
          close_piece();
        piece.skip++;
        break;
      }
    }
  }
  plan.dot_nl_ = dot != 0;
  if (!fits || (!plan.dot_nl_ && newline_literal))
    return NULL;
  close_segment();
  if (plan.segments_.size() > 2) {
    auto middle = std::remove_if(
        plan.segments_.begin() + 1, plan.segments_.end() - 1,
        [](const Segment& s) { return s.begin == s.end; });
    plan.segments_.erase(middle, plan.segments_.end() - 1);
  }
  for (Segment& segment : plan.segments_) {
    if (segment.begin == segment.end)
      continue;
    const Piece& head = plan.pieces_[segment.begin];
    segment.finder = LiteralFinder(
        absl::string_view(plan.bytes_.data() + head.offset, head.size));
  }
  return new SegmentPlan(std::move(plan));
}

size_t SegmentPlan::UnitAt(const char* p, const char* end) const {
  if (p == end)
    return 0;
  if (latin1_)
    return 1;
  const size_t n = UnitSize(static_cast<uint8_t>(*p));
  if (n == 0 || n > static_cast<size_t>(end - p))
    return 0;
  for (size_t i = 1; i < n; i++)
    if (!IsContinuation(static_cast<uint8_t>(p[i])))
      return 0;
  return n;
}

size_t SegmentPlan::UnitBefore(const char* begin, const char* p) const {
  if (p == begin)
    return 0;
  if (latin1_)
    return 1;
  const size_t available = static_cast<size_t>(p - begin);
  for (size_t n = 1; n <= 4 && n <= available; n++) {
    const uint8_t b = static_cast<uint8_t>(*(p - n));
    if (!IsContinuation(b))
      return UnitSize(b) == n ? n : 0;
  }
  return 0;
}

bool SegmentPlan::Units(absl::string_view text) const {
  if (latin1_)
    return true;
  const char* p = text.data();
  const char* end = p + text.size();
  while (p != end) {
#if defined(__clang__)
    using Block = signed char __attribute__((vector_size(32)));
    using Bits = bool __attribute__((ext_vector_type(32)));
    if (end - p >= 32) {
      Block block;
      memcpy(&block, p, sizeof(block));
      const Bits high = __builtin_convertvector(block < 0, Bits);
      uint32_t mask;
      memcpy(&mask, &high, sizeof(mask));
      if (mask == 0) {
        p += 32;
        continue;
      }
      p += __builtin_ctz(mask);
    }
#endif
    const size_t n = UnitAt(p, end);
    if (n == 0)
      return false;
    p += n;
  }
  return true;
}

size_t SegmentPlan::MatchAt(const Piece* piece, const Piece* end,
                            absl::string_view text, size_t pos,
                            size_t limit) const {
  const char* data = text.data();
  for (; piece != end; piece++) {
    for (uint32_t skip = piece->skip; skip != 0; skip--) {
      const size_t n = UnitAt(data + pos, data + limit);
      if (n == 0)
        return kNoMatch;
      pos += n;
    }
    const size_t size = piece->size;
    if (size > limit - pos ||
        !LiteralFinder::Equal(data + pos, bytes_.data() + piece->offset, size))
      return kNoMatch;
    pos += size;
  }
  return pos;
}

size_t SegmentPlan::MatchBefore(const Segment& segment, absl::string_view text,
                                size_t lower) const {
  const char* data = text.data();
  const Piece* first = pieces_.data() + segment.begin;
  size_t pos = text.size();
  for (const Piece* piece = pieces_.data() + segment.end; piece != first;) {
    piece--;
    const size_t size = piece->size;
    if (size > pos - lower)
      return kNoMatch;
    pos -= size;
    if (!LiteralFinder::Equal(data + pos, bytes_.data() + piece->offset, size))
      return kNoMatch;
    for (uint32_t skip = piece->skip; skip != 0; skip--) {
      const size_t n = UnitBefore(data + lower, data + pos);
      if (n == 0)
        return kNoMatch;
      pos -= n;
    }
  }
  return pos;
}

size_t SegmentPlan::FindFrom(const Segment& segment, absl::string_view text,
                             size_t pos, size_t limit) const {
  const Piece* first = pieces_.data() + segment.begin;
  const Piece* end = pieces_.data() + segment.end;
  if (first->size == 0)
    return MatchAt(first, end, text, pos, limit);
  const char* data = text.data();
  const absl::string_view literal(bytes_.data() + first->offset, first->size);
  for (size_t from = pos + first->skip;; from++) {
    if (from > limit)
      return kNoMatch;
    const char* hit = segment.finder.Find(literal, data + from, data + limit);
    if (hit == NULL)
      return kNoMatch;
    from = static_cast<size_t>(hit - data);
    size_t start = from;
    uint32_t skip = first->skip;
    for (; skip != 0; skip--) {
      const size_t n = UnitBefore(data + pos, data + start);
      if (n == 0)
        break;
      start -= n;
    }
    if (skip == 0) {
      const size_t matched =
          MatchAt(first + 1, end, text, from + first->size, limit);
      if (matched != kNoMatch)
        return matched;
    }
  }
}

bool SegmentPlan::Match(absl::string_view text) const {
  const Segment& head = segments_.front();
  const Piece* pieces = pieces_.data();
  size_t pos =
      MatchAt(pieces + head.begin, pieces + head.end, text, 0, text.size());
  if (pos == kNoMatch)
    return false;
  if (!any_string_) {
    if (pos != text.size())
      return false;
  } else {
    const size_t limit = MatchBefore(segments_.back(), text, pos);
    if (limit == kNoMatch)
      return false;
    for (size_t i = 1; i + 1 < segments_.size(); i++) {
      pos = FindFrom(segments_[i], text, pos, limit);
      if (pos == kNoMatch)
        return false;
    }
  }
  if (!dot_nl_ && memchr(text.data(), '\n', text.size()) != NULL)
    return false;
  return !any_string_ || Units(text);
}

}  // namespace re2
