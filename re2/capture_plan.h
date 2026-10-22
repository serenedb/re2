#ifndef RE2_CAPTURE_PLAN_H_
#define RE2_CAPTURE_PLAN_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "absl/strings/string_view.h"

namespace re2 {

class Regexp;

class CapturePlan {
 public:
  static CapturePlan* Make(Regexp* re, bool bytewise);

  bool Match(absl::string_view text, absl::string_view* submatch,
             int nsubmatch) const;

  int max_submatch() const { return 1 + ncap_; }

 private:
  static constexpr int kMaxCaptures = 9;
  static constexpr int kMaxOptionals = 8;

  enum class Op : uint8_t {
    kLiteral,
    kOptional,
    kRun,
    kCaptureBegin,
    kCaptureEnd,
  };

  struct Step {
    Op op;
    bool plus;
    int cap;
    uint32_t offset;
    uint32_t size;
    uint64_t bits[4];
  };

  struct State {
    const char* caps[2 * (kMaxCaptures + 1)];
  };

  CapturePlan() = default;

  bool Add(Regexp* re, bool bytewise);
  bool AddRun(Regexp* sub, bool plus, bool bytewise);
  void AddLiteral(Op op, const std::string& bytes);
  bool RunsStopOnce() const;
  bool MatchFrom(size_t i, const char* p, const char* end, State* state,
                 const char** match_end) const;

  static bool Contains(const uint64_t* bits, unsigned char c) {
    return ((bits[c >> 6] >> (c & 63)) & 1) != 0;
  }

  std::string bytes_;
  std::vector<Step> steps_;
  int ncap_ = 0;
  int optionals_ = 0;
};

}  // namespace re2

#endif  // RE2_CAPTURE_PLAN_H_
