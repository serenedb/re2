#include "re2/capture_plan.h"

#include <string.h>

#include <memory>

#include "re2/regexp.h"
#include "util/utf.h"

namespace re2 {

static void SetBit(uint64_t* bits, int c) {
  bits[c >> 6] |= uint64_t{1} << (c & 63);
}

static bool AsciiLiteral(Regexp* re, std::string* bytes) {
  if (re->parse_flags() & Regexp::FoldCase)
    return false;
  if (re->op() == kRegexpLiteral) {
    if (re->rune() >= 0x80)
      return false;
    bytes->push_back(static_cast<char>(re->rune()));
    return true;
  }
  if (re->op() == kRegexpLiteralString) {
    for (int i = 0; i < re->nrunes(); i++) {
      if (re->runes()[i] >= 0x80)
        return false;
      bytes->push_back(static_cast<char>(re->runes()[i]));
    }
    return true;
  }
  return false;
}

void CapturePlan::AddLiteral(Op op, const std::string& bytes) {
  if (op == Op::kLiteral && !steps_.empty() &&
      steps_.back().op == Op::kLiteral) {
    bytes_ += bytes;
    steps_.back().size += static_cast<uint32_t>(bytes.size());
    return;
  }
  Step step = {};
  step.op = op;
  step.offset = static_cast<uint32_t>(bytes_.size());
  step.size = static_cast<uint32_t>(bytes.size());
  bytes_ += bytes;
  steps_.push_back(step);
}

bool CapturePlan::AddRun(Regexp* sub, bool plus, bool bytewise) {
  Step step = {};
  step.op = Op::kRun;
  step.plus = plus;
  const bool latin1 = (sub->parse_flags() & Regexp::Latin1) != 0;
  const Rune top = latin1 ? 0xFF : Runemax;
  if (sub->op() == kRegexpLiteral) {
    if ((sub->parse_flags() & Regexp::FoldCase) || sub->rune() >= 0x80)
      return false;
    SetBit(step.bits, sub->rune());
  } else if (sub->op() == kRegexpAnyChar) {
    if (!latin1 && !bytewise)
      return false;
    for (int c = 0; c < 256; c++)
      SetBit(step.bits, c);
  } else if (sub->op() == kRegexpCharClass) {
    Rune wide = 0;
    for (CharClass::iterator i = sub->cc()->begin(); i != sub->cc()->end();
         ++i) {
      for (Rune r = i->lo; r <= i->hi && r < 0x80; r++)
        SetBit(step.bits, r);
      if (i->hi >= 0x80)
        wide += i->hi - (i->lo > 0x80 ? i->lo : 0x80) + 1;
    }
    if (wide != 0) {
      if (wide != top - 0x80 + 1 || (!latin1 && !bytewise))
        return false;
      for (int c = 0x80; c < 256; c++)
        SetBit(step.bits, c);
    }
  } else {
    return false;
  }
  steps_.push_back(step);
  return true;
}

bool CapturePlan::Add(Regexp* re, bool bytewise) {
  std::string bytes;
  switch (re->op()) {
    case kRegexpEmptyMatch:
      return true;
    case kRegexpLiteral:
    case kRegexpLiteralString:
      if (!AsciiLiteral(re, &bytes))
        return false;
      AddLiteral(Op::kLiteral, bytes);
      return true;
    case kRegexpConcat:
      for (int i = 0; i < re->nsub(); i++) {
        if (!Add(re->sub()[i], bytewise))
          return false;
      }
      return true;
    case kRegexpCapture: {
      if (re->cap() > kMaxCaptures)
        return false;
      Step begin = {};
      begin.op = Op::kCaptureBegin;
      begin.cap = re->cap();
      steps_.push_back(begin);
      if (!Add(re->sub()[0], bytewise))
        return false;
      Step end = begin;
      end.op = Op::kCaptureEnd;
      steps_.push_back(end);
      if (re->cap() > ncap_)
        ncap_ = re->cap();
      return true;
    }
    case kRegexpQuest:
      if ((re->parse_flags() & Regexp::NonGreedy) ||
          !AsciiLiteral(re->sub()[0], &bytes) || ++optionals_ > kMaxOptionals)
        return false;
      AddLiteral(Op::kOptional, bytes);
      return true;
    case kRegexpStar:
    case kRegexpPlus:
      if (re->parse_flags() & Regexp::NonGreedy)
        return false;
      return AddRun(re->sub()[0], re->op() == kRegexpPlus, bytewise);
    default:
      return false;
  }
}

bool CapturePlan::RunsStopOnce() const {
  for (size_t i = 0; i < steps_.size(); i++) {
    if (steps_[i].op != Op::kRun)
      continue;
    const uint64_t* run = steps_[i].bits;
    for (size_t j = i + 1; j < steps_.size(); j++) {
      const Step& next = steps_[j];
      if (next.op == Op::kCaptureBegin || next.op == Op::kCaptureEnd)
        continue;
      if (next.op == Op::kRun) {
        for (int w = 0; w < 4; w++) {
          if (run[w] & next.bits[w])
            return false;
        }
        if (next.plus)
          break;
        continue;
      }
      if (Contains(run, static_cast<unsigned char>(bytes_[next.offset])))
        return false;
      if (next.op == Op::kLiteral)
        break;
    }
  }
  return true;
}

CapturePlan* CapturePlan::Make(Regexp* re, bool bytewise) {
  std::unique_ptr<CapturePlan> plan(new CapturePlan);
  if (!plan->Add(re, bytewise) || plan->steps_.empty() ||
      !plan->RunsStopOnce())
    return NULL;
  for (const Step& step : plan->steps_) {
    if ((step.op == Op::kLiteral || step.op == Op::kOptional) &&
        step.size == 0)
      return NULL;
  }
  return plan.release();
}

bool CapturePlan::MatchFrom(size_t i, const char* p, const char* end,
                            State* state, const char** match_end) const {
  for (; i < steps_.size(); i++) {
    const Step& step = steps_[i];
    switch (step.op) {
      case Op::kLiteral:
        if (static_cast<size_t>(end - p) < step.size ||
            memcmp(p, bytes_.data() + step.offset, step.size) != 0)
          return false;
        p += step.size;
        break;
      case Op::kOptional:
        if (static_cast<size_t>(end - p) >= step.size &&
            memcmp(p, bytes_.data() + step.offset, step.size) == 0) {
          State saved = *state;
          if (MatchFrom(i + 1, p + step.size, end, state, match_end))
            return true;
          *state = saved;
        }
        break;
      case Op::kRun: {
        const char* q = p;
        while (q < end && Contains(step.bits, static_cast<unsigned char>(*q)))
          q++;
        if (step.plus && q == p)
          return false;
        p = q;
        break;
      }
      case Op::kCaptureBegin:
        state->caps[2 * step.cap] = p;
        break;
      case Op::kCaptureEnd:
        state->caps[2 * step.cap + 1] = p;
        break;
    }
  }
  *match_end = p;
  return true;
}

bool CapturePlan::Match(absl::string_view text, absl::string_view* submatch,
                        int nsubmatch) const {
  State state;
  for (int i = 0; i < 2 * (ncap_ + 1); i++)
    state.caps[i] = NULL;
  const char* begin = text.data();
  const char* match_end = NULL;
  if (!MatchFrom(0, begin, begin + text.size(), &state, &match_end))
    return false;
  if (nsubmatch > 0)
    submatch[0] = absl::string_view(begin, static_cast<size_t>(match_end - begin));
  for (int i = 1; i < nsubmatch; i++) {
    const char* b = i <= ncap_ ? state.caps[2 * i] : NULL;
    const char* e = i <= ncap_ ? state.caps[2 * i + 1] : NULL;
    submatch[i] = b == NULL || e == NULL
                      ? absl::string_view()
                      : absl::string_view(b, static_cast<size_t>(e - b));
  }
  return true;
}

}  // namespace re2
