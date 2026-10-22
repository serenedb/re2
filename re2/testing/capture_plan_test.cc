#include "re2/capture_plan.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "re2/prog.h"
#include "re2/regexp.h"

namespace re2 {

namespace {

struct Pattern {
  const char* regexp;
  bool bytewise;
};

const Pattern kPatterns[] = {
    {"https?://(?:www\\.)?([^/]+)/", true},
    {"a?b?(c+)d", false},
    {"(x*)(y+)z", false},
    {"([a-z]+)@([a-z.]+)", false},
    {"(?:ab)?(?:ab)?(c)", false},
    {"([^/]+)/(\\d+)", true},
    {"s?(:)//", false},
    {"(w*)(\\.)", false},
    {"([^:]*):", true},
};

const char* kUnits[] = {"a", "b", "c", "d", "s", "w", "x", "y", "z", ":",
                        "/", ".", "@", "1", "9", "https://", "www.", "ab",
                        "\xc3\xa9", "\xe2\x82\xac"};

std::string RandomText(uint64_t* seed) {
  std::string text;
  *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
  int units = static_cast<int>((*seed >> 33) % 12);
  for (int i = 0; i < units; i++) {
    *seed = *seed * 6364136223846793005ULL + 1442695040888963407ULL;
    text += kUnits[(*seed >> 33) % (sizeof(kUnits) / sizeof(kUnits[0]))];
  }
  return text;
}

TEST(CapturePlan, MatchesBacktracking) {
  for (const Pattern& pattern : kPatterns) {
    Regexp* re = Regexp::Parse(pattern.regexp, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL) << pattern.regexp;
    std::unique_ptr<CapturePlan> plan(CapturePlan::Make(re, pattern.bytewise));
    ASSERT_TRUE(plan != NULL) << pattern.regexp;
    std::unique_ptr<Prog> prog(re->CompileToProg(0));
    ASSERT_TRUE(prog != NULL) << pattern.regexp;
    const int ncap = 1 + re->NumCaptures();
    uint64_t seed = 42;
    for (int iteration = 0; iteration < 20000; iteration++) {
      const std::string text = RandomText(&seed);
      std::vector<absl::string_view> expected(ncap);
      std::vector<absl::string_view> actual(ncap);
      const bool want = prog->UnsafeSearchBacktrack(
          text, text, Prog::kAnchored, Prog::kFirstMatch, expected.data(),
          ncap);
      const bool got = plan->Match(text, actual.data(), ncap);
      ASSERT_EQ(want, got) << pattern.regexp << " on " << absl::CEscape(text);
      if (!want)
        continue;
      for (int i = 0; i < ncap; i++) {
        ASSERT_EQ(expected[i].data(), actual[i].data())
            << pattern.regexp << " group " << i << " on "
            << absl::CEscape(text);
        ASSERT_EQ(expected[i].size(), actual[i].size())
            << pattern.regexp << " group " << i << " on "
            << absl::CEscape(text);
      }
    }
    re->Decref();
  }
}

TEST(CapturePlan, RejectsShapesItCannotMatchWithoutBacktracking) {
  const char* rejected[] = {"(a+)a",  "([ab]*)b", "(.*)/",           "a|b",
                            "(a+?)b", "[^/]+/",   "(?i)k+", "(w*)(?:ww)?(\\.)",
                            "(ab)?c"};
  for (const char* pattern : rejected) {
    Regexp* re = Regexp::Parse(pattern, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL) << pattern;
    std::unique_ptr<CapturePlan> plan(CapturePlan::Make(re, false));
    EXPECT_TRUE(plan == NULL) << pattern;
    re->Decref();
  }
}

}  // namespace

}  // namespace re2
