// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stdint.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "re2/prog.h"
#include "re2/re2.h"
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
    {"(a+)a", false},
    {"([ab]*)b", false},
    {"(.*)/", false},
    {"(a)|(b)", false},
    {"(a+?)b", false},
    {"(?i)(k+)", false},
    {"(w*)(?:ww)?(\\.)", false},
    {"(ab)?c", false},
    {"\\b(\\w+)\\b", false},
    {"(?m)^(\\w*):", false},
    {"(\\d+)$", false},
    {"\\B(b+)", false},
    {"^https?://(?:www\\.)?([^/]+)/.*$", true},
    {"(.*)\xc3\xa9(.*)", true},
    {"(.+)\xe2\x82\xac", true},
    {"([^/]*)\xc3\xa9", true},
    {"(.{2})\xe2\x82\xac", true},
    {"(?s)(.*)/(.*)", true},
};

const char* kUnits[] = {"a", "b", "c", "d", "s", "w", "x", "y", "z", ":",
                        "/", ".", "@", "1", "9", "https://", "www.", "ab",
                        "\n", " ", "k", "K", "\xc3\xa9", "\xe2\x82\xac"};

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

struct Mode {
  RE2::Anchor re_anchor;
  Prog::Anchor anchor;
  Prog::MatchKind kind;
};

const Mode kModes[] = {
    {RE2::UNANCHORED, Prog::kUnanchored, Prog::kFirstMatch},
    {RE2::ANCHOR_START, Prog::kAnchored, Prog::kFirstMatch},
    {RE2::ANCHOR_BOTH, Prog::kAnchored, Prog::kFullMatch},
};

void ExpectSame(const char* regexp, const std::string& text, int ncap,
                bool want, const std::vector<absl::string_view>& expected,
                bool got, const std::vector<absl::string_view>& actual) {
  ASSERT_EQ(want, got) << regexp << " on " << absl::CEscape(text);
  if (!want)
    return;
  for (int i = 0; i < ncap; i++) {
    ASSERT_EQ(expected[i].data(), actual[i].data())
        << regexp << " group " << i << " on " << absl::CEscape(text);
    ASSERT_EQ(expected[i].size(), actual[i].size())
        << regexp << " group " << i << " on " << absl::CEscape(text);
  }
}

std::unique_ptr<Prog> Compile(Regexp* re) {
  std::unique_ptr<Prog> prog(re->CompileToProg(0));
  if (prog != NULL) {
    prog->IsOnePass();
    prog->ReserveTDFA();
  }
  return prog;
}

TEST(TDFA, MatchesBacktracking) {
  for (const Pattern& pattern : kPatterns) {
    Regexp* re = Regexp::Parse(pattern.regexp, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL) << pattern.regexp;
    std::unique_ptr<Prog> prog = Compile(re);
    ASSERT_TRUE(prog != NULL) << pattern.regexp;
    const int ncap = 1 + re->NumCaptures();
    for (const Mode& mode : kModes) {
      uint64_t seed = 42;
      for (int iteration = 0; iteration < 20000; iteration++) {
        const std::string text = RandomText(&seed) + RandomText(&seed);
        std::vector<absl::string_view> expected(ncap);
        std::vector<absl::string_view> actual(ncap);
        const bool want = prog->UnsafeSearchBacktrack(
            text, text, mode.anchor, mode.kind, expected.data(), ncap);
        const int found = prog->SearchTDFA(text, text, mode.anchor, mode.kind,
                                           actual.data(), ncap);
        ASSERT_GE(found, 0) << pattern.regexp;
        ExpectSame(pattern.regexp, text, ncap, want, expected, found == 1,
                   actual);
      }
    }
    re->Decref();
  }
}

TEST(TDFA, RE2MatchAgreesWithBacktracking) {
  for (const Pattern& pattern : kPatterns) {
    RE2::Options options;
    options.set_valid_utf8(pattern.bytewise);
    RE2 re2(pattern.regexp, options);
    ASSERT_TRUE(re2.ok()) << pattern.regexp;
    Regexp* re = Regexp::Parse(pattern.regexp, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL) << pattern.regexp;
    std::unique_ptr<Prog> prog(re->CompileToProg(0));
    ASSERT_TRUE(prog != NULL) << pattern.regexp;
    const int ncap = 1 + re->NumCaptures();
    for (const Mode& mode : kModes) {
      uint64_t seed = 7;
      for (int iteration = 0; iteration < 20000; iteration++) {
        const std::string text = RandomText(&seed) + RandomText(&seed);
        std::vector<absl::string_view> expected(ncap);
        std::vector<absl::string_view> actual(ncap);
        const bool want = prog->UnsafeSearchBacktrack(
            text, text, mode.anchor, mode.kind, expected.data(), ncap);
        const bool got = re2.Match(text, 0, text.size(), mode.re_anchor,
                                   actual.data(), ncap);
        ExpectSame(pattern.regexp, text, ncap, want, expected, got, actual);
      }
    }
    re->Decref();
  }
}

TEST(TDFA, ConcurrentSearchesShareTheAutomaton) {
  for (const Pattern& pattern : kPatterns) {
    Regexp* re = Regexp::Parse(pattern.regexp, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL) << pattern.regexp;
    std::unique_ptr<Prog> prog = Compile(re);
    ASSERT_TRUE(prog != NULL) << pattern.regexp;
    const int ncap = 1 + re->NumCaptures();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++) {
      threads.emplace_back([&, t] {
        uint64_t seed = 100 + t;
        for (int iteration = 0; iteration < 2000; iteration++) {
          const std::string text = RandomText(&seed) + RandomText(&seed);
          std::vector<absl::string_view> expected(ncap);
          std::vector<absl::string_view> actual(ncap);
          const bool want = prog->UnsafeSearchBacktrack(
              text, text, Prog::kUnanchored, Prog::kFirstMatch,
              expected.data(), ncap);
          const int found =
              prog->SearchTDFA(text, text, Prog::kUnanchored,
                               Prog::kFirstMatch, actual.data(), ncap);
          ASSERT_GE(found, 0) << pattern.regexp;
          ExpectSame(pattern.regexp, text, ncap, want, expected, found == 1,
                     actual);
        }
      });
    }
    for (std::thread& thread : threads)
      thread.join();
    re->Decref();
  }
}

TEST(TDFA, DeclinesWithoutMemoryAndRE2StillMatches) {
  Regexp* re = Regexp::Parse("([a-z]+)@([a-z.]+)", Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);
  std::unique_ptr<Prog> prog(re->CompileToProg(0));
  ASSERT_TRUE(prog != NULL);
  std::unique_ptr<Prog> starved(re->CompileToProg(0));
  ASSERT_TRUE(starved != NULL);
  starved->set_dfa_mem(0);
  starved->ReserveTDFA();
  const std::string text = "mail to john@example.org now";
  absl::string_view actual[3];
  EXPECT_EQ(-1, starved->SearchTDFA(text, text, Prog::kUnanchored,
                                    Prog::kFirstMatch, actual, 3));

  RE2::Options options;
  options.set_max_mem(1 << 12);
  RE2 small("([a-z]+)@([a-z.]+)", options);
  ASSERT_TRUE(small.ok());
  absl::string_view expected[3];
  ASSERT_TRUE(prog->UnsafeSearchBacktrack(text, text, Prog::kUnanchored,
                                          Prog::kFirstMatch, expected, 3));
  ASSERT_TRUE(small.Match(text, 0, text.size(), RE2::UNANCHORED, actual, 3));
  for (int i = 0; i < 3; i++)
    EXPECT_EQ(expected[i], actual[i]) << i;
  re->Decref();
}

}  // namespace

}  // namespace re2
