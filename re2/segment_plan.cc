// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/segment_plan.h"

#include <string.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "absl/base/attributes.h"
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

constexpr Rune kTwoByteEnd = 0x800;

Rune FoldBase(Rune r) {
  Rune base = r;
  for (Rune x = CycleFoldRune(r); x != r; x = CycleFoldRune(x))
    base = std::min(base, x);
  return 'A' <= base && base <= 'Z' ? base + ('a' - 'A') : base;
}

const uint16_t* TwoByteBases() {
  static const std::array<uint16_t, kTwoByteEnd - Runeself> kBases = [] {
    std::array<uint16_t, kTwoByteEnd - Runeself> bases;
    for (Rune r = Runeself; r < kTwoByteEnd; r++)
      bases[r - Runeself] = static_cast<uint16_t>(FoldBase(r));
    return bases;
  }();
  return kBases.data();
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline char LowerAscii(uint8_t b) {
  return static_cast<char>(
      b + (static_cast<uint8_t>(b - 'A') < 26 ? 'a' - 'A' : 0));
}

size_t AsciiPrefix(const char* p, size_t n) {
  size_t i = 0;
  for (uint64_t word; i + 8 <= n; i += 8) {
    memcpy(&word, p + i, sizeof(word));
    if ((word & 0x8080808080808080ull) != 0)
      break;
  }
  while (i < n && static_cast<uint8_t>(p[i]) < Runeself)
    i++;
  return i;
}

template <bool kIgnoreCase>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline bool Same(const char* text,
                                              const char* literal, size_t n) {
  if constexpr (kIgnoreCase)
    return LiteralFinder::EqualIgnoreCase(text, literal, n);
  else
    return LiteralFinder::Equal(text, literal, n);
}

template <bool kIgnoreCase>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline const char* FindLiteral(
    const LiteralFinder& finder, absl::string_view literal, const char* p,
    const char* end) {
  if constexpr (kIgnoreCase)
    return finder.FindIgnoreCase(literal, p, end);
  else
    return finder.Find(literal, p, end);
}

bool IsFoldCycle(const Rune* set, int n) {
  int count = 0;
  Rune x = set[0];
  do {
    if (std::find(set, set + n, x) == set + n)
      return false;
    count++;
    x = CycleFoldRune(x);
  } while (x != set[0]);
  return count == n;
}

int ChooseFold(Regexp** subs, int first, int last, bool latin1) {
  bool exact = true;
  bool cycles = !latin1;
  Rune set[4];
  auto narrow = [&](int n) {
    exact = exact && n == 1;
    cycles = cycles && IsFoldCycle(set, n);
    return exact || cycles;
  };
  for (int i = first; i < last; i++) {
    Regexp* sub = subs[i];
    if (sub->op() == kRegexpLiteral || sub->op() == kRegexpLiteralString) {
      const bool fold = (sub->parse_flags() & Regexp::FoldCase) != 0;
      const int nrunes = sub->op() == kRegexpLiteral ? 1 : sub->nrunes();
      for (int j = 0; j < nrunes; j++) {
        int n = 0;
        set[n++] = sub->op() == kRegexpLiteral ? sub->rune() : sub->runes()[j];
        if (fold && 'a' <= set[0] && set[0] <= 'z')
          set[n++] = set[0] - ('a' - 'A');
        if (!narrow(n))
          return -1;
      }
    } else if (sub->op() == kRegexpCharClass && !IsAnyExceptNewline(sub)) {
      CharClass* cc = sub->cc();
      if (cc->size() == 0 || cc->size() > 4)
        return -1;
      int n = 0;
      for (CharClass::iterator it = cc->begin(); it != cc->end(); ++it)
        for (Rune r = it->lo; r <= it->hi; r++)
          set[n++] = r;
      if (!narrow(n))
        return -1;
    }
  }
  return exact ? 0 : 1;
}

}  // namespace

void SegmentPlan::ClosePiece(Piece* piece) {
  if (piece->skip != 0 || piece->size != 0)
    pieces_.push_back(*piece);
  *piece = {0, static_cast<uint32_t>(bytes_.size()), 0};
}

void SegmentPlan::CloseSegment(Piece* piece, uint32_t* begin) {
  ClosePiece(piece);
  const uint32_t end = static_cast<uint32_t>(pieces_.size());
  segments_.push_back({*begin, end, LiteralFinder()});
  *begin = end;
}

SegmentPlan* SegmentPlan::Make(Regexp* re, bool valid_utf8) {
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
  plan.valid_utf8_ = valid_utf8;
  const int fold = ChooseFold(subs, first, last, plan.latin1_);
  if (fold < 0)
    return NULL;
  if (fold == 1)
    plan.bases_ = TwoByteBases();
  int dot = -1;
  bool newline_literal = false;
  Piece piece{0, 0, 0};
  uint32_t begin = 0;
  auto unit_kind = [&](Regexp* sub) {
    if (sub->op() == kRegexpAnyChar)
      return 1;
    if (IsAnyExceptNewline(sub))
      return 0;
    return -1;
  };
  auto append = [&](Rune r) {
    if (plan.bases_ != NULL)
      r = FoldBase(r);
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
        append(sub->rune());
        break;
      case kRegexpLiteralString:
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
        plan.CloseSegment(&piece, &begin);
        plan.any_string_ = true;
        break;
      }
      default: {
        const int kind = unit_kind(sub);
        if (kind < 0 && sub->op() == kRegexpCharClass) {
          append(sub->cc()->begin()->lo);
          break;
        }
        if (kind < 0 || (dot >= 0 && kind != dot)) {
          fits = false;
          break;
        }
        dot = kind;
        if (piece.size != 0)
          plan.ClosePiece(&piece);
        piece.skip++;
        break;
      }
    }
  }
  plan.dot_nl_ = dot != 0;
  if (!fits || (!plan.dot_nl_ && newline_literal))
    return NULL;
  plan.CloseSegment(&piece, &begin);
  return Finish(std::move(plan));
}

SegmentPlan* SegmentPlan::MakeLike(absl::string_view pattern, bool has_escape,
                                   char escape, bool valid_utf8) {
  SegmentPlan plan;
  plan.valid_utf8_ = valid_utf8;
  Piece piece{0, 0, 0};
  uint32_t begin = 0;
  for (size_t i = 0; i < pattern.size(); i++) {
    char c = pattern[i];
    if (has_escape && c == escape) {
      if (++i == pattern.size())
        return NULL;
      c = pattern[i];
    } else if (c == '%') {
      plan.CloseSegment(&piece, &begin);
      plan.any_string_ = true;
      continue;
    } else if (c == '_') {
      if (piece.size != 0)
        plan.ClosePiece(&piece);
      piece.skip++;
      continue;
    }
    plan.bytes_ += c;
    piece.size++;
  }
  plan.CloseSegment(&piece, &begin);
  return Finish(std::move(plan));
}

SegmentPlan* SegmentPlan::Finish(SegmentPlan plan) {
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
  auto literal = [&](const Segment& segment, uint32_t* offset,
                     uint32_t* size) {
    if (segment.begin == segment.end)
      return true;
    const Piece& p = plan.pieces_[segment.begin];
    if (segment.end - segment.begin != 1 || p.skip != 0)
      return false;
    *offset = p.offset;
    *size = p.size;
    return true;
  };
  const std::vector<Segment>& segments = plan.segments_;
  if (!plan.any_string_) {
    if (literal(segments.front(), &plan.first_, &plan.first_size_))
      plan.shape_ = Shape::kExact;
  } else if (segments.size() == 2) {
    if (literal(segments.front(), &plan.first_, &plan.first_size_) &&
        literal(segments.back(), &plan.last_, &plan.last_size_))
      plan.shape_ = Shape::kAffix;
  } else if (segments.size() == 3 && segments[0].begin == segments[0].end &&
             segments[2].begin == segments[2].end &&
             literal(segments[1], &plan.first_, &plan.first_size_)) {
    plan.shape_ = Shape::kContains;
  } else {
    uint32_t offset = 0;
    uint32_t size = 0;
    bool literals =
        literal(segments.front(), &plan.first_, &plan.first_size_) &&
        literal(segments.back(), &plan.last_, &plan.last_size_);
    for (size_t i = 1; literals && i + 1 < segments.size(); i++)
      literals = literal(segments[i], &offset, &size);
    if (literals)
      plan.shape_ = Shape::kSegments;
  }
  for (const Piece& piece : plan.pieces_)
    plan.min_size_ += piece.size + piece.skip;
  SegmentPlan* result = new SegmentPlan(std::move(plan));
  result->literals_ = result->bytes_.data();
  if (result->bases_ != NULL) {
    result->folded_shape_ = result->shape_;
    result->shape_ = Shape::kFolded;
  }
  return result;
}

ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t SegmentPlan::UnitAt(
    const char* p, const char* end) const {
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

ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t SegmentPlan::UnitBefore(
    const char* begin, const char* p) const {
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
  if (latin1_ || valid_utf8_)
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

template <bool kIgnoreCase>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t SegmentPlan::MatchAt(
    const Piece* piece, const Piece* end, absl::string_view text, size_t pos,
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
        !Same<kIgnoreCase>(data + pos, bytes_.data() + piece->offset, size))
      return kNoMatch;
    pos += size;
  }
  return pos;
}

template <bool kIgnoreCase>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline size_t SegmentPlan::MatchBefore(
    const Segment& segment, absl::string_view text, size_t lower) const {
  const char* data = text.data();
  const Piece* first = pieces_.data() + segment.begin;
  size_t pos = text.size();
  for (const Piece* piece = pieces_.data() + segment.end; piece != first;) {
    piece--;
    const size_t size = piece->size;
    if (size > pos - lower)
      return kNoMatch;
    pos -= size;
    if (!Same<kIgnoreCase>(data + pos, bytes_.data() + piece->offset, size))
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

template <bool kIgnoreCase>
size_t SegmentPlan::FindFrom(const Segment& segment, absl::string_view text,
                             size_t pos, size_t limit) const {
  const Piece* first = pieces_.data() + segment.begin;
  const Piece* end = pieces_.data() + segment.end;
  if (first->size == 0)
    return MatchAt<kIgnoreCase>(first, end, text, pos, limit);
  const char* data = text.data();
  const absl::string_view literal(bytes_.data() + first->offset, first->size);
  for (size_t from = pos + first->skip;; from++) {
    if (from > limit)
      return kNoMatch;
    const char* hit = FindLiteral<kIgnoreCase>(segment.finder, literal,
                                               data + from, data + limit);
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
          MatchAt<kIgnoreCase>(first + 1, end, text, from + first->size, limit);
      if (matched != kNoMatch)
        return matched;
    }
  }
}

size_t SegmentPlan::Fold(absl::string_view text, char* out) const {
  const char* p = text.data();
  const char* end = text.data() + text.size();
  char* o = out;
  while (p != end) {
    const uint8_t b = static_cast<uint8_t>(*p);
    if (b < Runeself) {
      const size_t n = AsciiPrefix(p, static_cast<size_t>(end - p));
      for (size_t i = 0; i < n; i++)
        o[i] = LowerAscii(static_cast<uint8_t>(p[i]));
      p += n;
      o += n;
      continue;
    }
    if (b >= 0xC2 && b < 0xE0 && end - p >= 2 &&
        IsContinuation(static_cast<uint8_t>(p[1]))) {
      const Rune base =
          bases_[(((b & 0x1F) << 6) | (static_cast<uint8_t>(p[1]) & 0x3F)) -
                 Runeself];
      if (base < Runeself) {
        *o++ = static_cast<char>(base);
      } else {
        o[0] = static_cast<char>(0xC0 | (base >> 6));
        o[1] = static_cast<char>(0x80 | (base & 0x3F));
        o += 2;
      }
      p += 2;
      continue;
    }
    const size_t n = UnitAt(p, end);
    if (n == 0)
      return kNoMatch;
    Rune r;
    chartorune(&r, p);
    const Rune base = FoldBase(r);
    if (base == r) {
      memcpy(o, p, n);
      o += n;
    } else {
      o += runetochar(o, &base);
    }
    p += n;
  }
  return static_cast<size_t>(o - out);
}

template <bool kIgnoreCase>
bool SegmentPlan::MatchGeneral(absl::string_view text) const {
  const Segment& head = segments_.front();
  const Piece* pieces = pieces_.data();
  size_t pos = MatchAt<kIgnoreCase>(pieces + head.begin, pieces + head.end,
                                    text, 0, text.size());
  if (pos == kNoMatch)
    return false;
  if (!any_string_) {
    if (pos != text.size())
      return false;
  } else {
    const size_t limit = MatchBefore<kIgnoreCase>(segments_.back(), text, pos);
    if (limit == kNoMatch)
      return false;
    for (size_t i = 1; i + 1 < segments_.size(); i++) {
      pos = FindFrom<kIgnoreCase>(segments_[i], text, pos, limit);
      if (pos == kNoMatch)
        return false;
    }
  }
  if (!dot_nl_ && text.find('\n') != absl::string_view::npos)
    return false;
  return !any_string_ || Units(text);
}

template <bool kIgnoreCase>
ABSL_ATTRIBUTE_ALWAYS_INLINE inline bool SegmentPlan::MatchShape(
    Shape shape, absl::string_view text) const {
  const char* data = text.data();
  const size_t size = text.size();
  switch (shape) {
    case Shape::kFolded:
      return MatchFolded(text);
    case Shape::kGeneral:
      return MatchGeneral<kIgnoreCase>(text);
    case Shape::kExact:
      return size == first_size_ &&
             Same<kIgnoreCase>(data, literals_ + first_, size);
    case Shape::kAffix:
      if (!Same<kIgnoreCase>(data, literals_ + first_, first_size_) ||
          !Same<kIgnoreCase>(data + size - last_size_, literals_ + last_,
                             last_size_))
        return false;
      break;
    case Shape::kContains:
      if (FindLiteral<kIgnoreCase>(
              segments_[1].finder,
              absl::string_view(literals_ + first_, first_size_), data,
              data + size) == NULL)
        return false;
      break;
    case Shape::kSegments: {
      if (!Same<kIgnoreCase>(data, literals_ + first_, first_size_) ||
          !Same<kIgnoreCase>(data + size - last_size_, literals_ + last_,
                             last_size_))
        return false;
      const char* p = data + first_size_;
      const char* limit = data + size - last_size_;
      for (size_t i = 1; i + 1 < segments_.size(); i++) {
        const Segment& segment = segments_[i];
        const Piece& piece = pieces_[segment.begin];
        if (static_cast<size_t>(limit - p) < piece.size)
          return false;
        const char* at = FindLiteral<kIgnoreCase>(
            segment.finder,
            absl::string_view(literals_ + piece.offset, piece.size), p, limit);
        if (at == NULL)
          return false;
        p = at + piece.size;
      }
      break;
    }
  }
  if (!dot_nl_ && text.find('\n') != absl::string_view::npos)
    return false;
  return Units(text);
}

bool SegmentPlan::Match(absl::string_view text) const {
  if (text.size() < min_size_)
    return false;
  return MatchShape<false>(shape_, text);
}

ABSL_ATTRIBUTE_NOINLINE bool SegmentPlan::MatchFolded(
    absl::string_view text) const {
  if (AsciiPrefix(text.data(), text.size()) == text.size())
    return MatchShape<true>(folded_shape_, text);
  char stack[512];
  std::unique_ptr<char[]> heap;
  char* out = stack;
  if (text.size() > sizeof(stack)) {
    heap.reset(new char[text.size()]);
    out = heap.get();
  }
  const size_t size = Fold(text, out);
  if (size == kNoMatch || size < min_size_)
    return false;
  return MatchShape<false>(folded_shape_, absl::string_view(out, size));
}

}  // namespace re2
