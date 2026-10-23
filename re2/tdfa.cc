// Copyright 2026 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/numeric/bits.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "re2/byte_set_finder.h"
#include "re2/prog.h"
#include "re2/sparse_set.h"

namespace re2 {

namespace {

constexpr int16_t kUnset = -1;
constexpr int16_t kNow = -2;

constexpr uint8_t kInjectNone = 0;
constexpr uint8_t kInjectOnce = 1;
constexpr uint8_t kInjectAlways = 2;

constexpr uint8_t kCtxBegin = 1;
constexpr uint8_t kCtxNewline = 2;
constexpr uint8_t kCtxWord = 4;

struct TState;

struct TEdge {
  TState* to;
  int nregs;
  bool match;

  int16_t* src() { return reinterpret_cast<int16_t*>(this + 1); }
  const int16_t* src() const {
    return reinterpret_cast<const int16_t*>(this + 1);
  }
  const int16_t* match_slots() const { return src() + nregs; }
};

struct EdgeDeleter {
  void operator()(TEdge* e) const { ::operator delete(e); }
};

constexpr uint8_t kRunUnknown = 0;
constexpr uint8_t kRunNone = 1;
constexpr uint8_t kRunToEnd = 2;
constexpr uint8_t kRunFind = 3;
constexpr uint8_t kRunSkip = 4;

constexpr uintptr_t kEdgeTag = 1;
constexpr uintptr_t kSpecialTag = 2;
constexpr uintptr_t kRunTag = 4;
constexpr uintptr_t kTagMask = kEdgeTag | kSpecialTag | kRunTag;
constexpr uintptr_t kUnbuilt = kEdgeTag;

struct TState {
  std::vector<int> roots;
  std::vector<int16_t> slots;
  int nregs = 0;
  uint8_t inject = kInjectNone;
  uint8_t ctx = 0;
  bool dead = false;
  bool idle = false;
  std::atomic<uint8_t> run{kRunUnknown};
  ByteSetFinder run_finder;

  std::atomic<uintptr_t>* next() {
    return reinterpret_cast<std::atomic<uintptr_t>*>(this + 1);
  }
};

struct StateDeleter {
  void operator()(TState* s) const {
    s->~TState();
    ::operator delete(s);
  }
};

TState* Untag(uintptr_t next) {
  return reinterpret_cast<TState*>(next & ~kTagMask);
}

const TEdge* UntagEdge(uintptr_t next) {
  return reinterpret_cast<const TEdge*>(next & ~kTagMask);
}

ABSL_ATTRIBUTE_NOINLINE const char* Run(const TState* s, uint8_t run,
                                        const char* p, const char* end) {
  switch (run) {
    case kRunToEnd:
      return end;
    case kRunFind: {
      const char* q = s->run_finder.Find(p, end);
      return q == NULL ? end : q;
    }
    case kRunSkip:
      return s->run_finder.Skip(p, end);
    default:
      return p;
  }
}

inline TState* StepPlain(TState* s, const char** pp, const char* end,
                         const uint8_t* bytemap, uintptr_t* out) {
  const char* p = *pp;
  uintptr_t next = 0;
  while (p != end) {
    next = s->next()[bytemap[static_cast<uint8_t>(*p)]].load(
        std::memory_order_acquire);
    if ((next & kTagMask) == 0) {
      s = reinterpret_cast<TState*>(next);
      ++p;
      next = 0;
      continue;
    }
    if ((next & kTagMask) != kRunTag)
      break;
    ++p;
    p = Run(s, s->run.load(std::memory_order_acquire), p, end);
    next = 0;
  }
  *pp = p;
  *out = next;
  return s;
}

}  // namespace

class TDFA {
 public:
  TDFA(Prog* prog, int nslots, bool full, std::atomic<int64_t>* budget);

  int Search(absl::string_view text, absl::string_view context, bool anchored,
             bool earliest, absl::string_view* match, int nmatch);

 private:
  struct Expansion {
    std::vector<int> roots;
    std::vector<int16_t> slots;
    bool match = false;
    bool cut = false;
    std::vector<int16_t> match_slots;
  };
  struct StackEntry {
    int id;
    int slot;
    int16_t old;
  };

  uintptr_t Start(uint8_t inject, uint8_t ctx);
  uintptr_t Next(TState* s, int index, int c);
  uintptr_t NextLocked(TState* s, int index, int c)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void ComputeRun(TState* s) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  void Expand(int id0, int16_t* slots, uint32_t flags, int c, Expansion* x)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  uintptr_t Publish(const Expansion& x, uint8_t inject, uint8_t ctx,
                    int src_nregs, bool final)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  bool Charge(int64_t cost) ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_);
  uint32_t Flags(uint8_t ctx, int c) const;
  uint8_t ContextAfter(int c) const;
  void Record(const int16_t* slots, const char* const* regs, const char* p,
              absl::string_view* match, int nmatch) const;

  Prog* prog_;
  int nslots_;
  bool full_;
  bool has_empty_;
  int nclass_;
  std::atomic<bool> failed_{false};
  std::atomic<uintptr_t> start_[3][8] = {};
  int class_byte_[256];

  absl::Mutex mu_;
  std::atomic<int64_t>* budget_;
  absl::flat_hash_map<std::string, TState*> states_ ABSL_GUARDED_BY(mu_);
  std::vector<std::unique_ptr<TState, StateDeleter>> owned_states_
      ABSL_GUARDED_BY(mu_);
  std::vector<std::unique_ptr<TEdge, EdgeDeleter>> owned_edges_
      ABSL_GUARDED_BY(mu_);
  SparseSet visited_ ABSL_GUARDED_BY(mu_);
  std::vector<StackEntry> stack_ ABSL_GUARDED_BY(mu_);
};

TDFA::TDFA(Prog* prog, int nslots, bool full, std::atomic<int64_t>* budget)
    : prog_(prog),
      nslots_(nslots),
      full_(full),
      has_empty_(prog->inst_count(kInstEmptyWidth) != 0),
      nclass_(prog->bytemap_range()),
      budget_(budget),
      visited_(prog->size()) {
  for (int c = 255; c >= 0; c--)
    class_byte_[prog->bytemap()[c]] = c;
}

uint32_t TDFA::Flags(uint8_t ctx, int c) const {
  uint32_t flags = 0;
  if (ctx & kCtxBegin)
    flags |= kEmptyBeginText | kEmptyBeginLine;
  if (ctx & kCtxNewline)
    flags |= kEmptyBeginLine;
  if (c < 0)
    flags |= kEmptyEndText | kEmptyEndLine;
  else if (c == '\n')
    flags |= kEmptyEndLine;
  bool before = (ctx & kCtxWord) != 0;
  bool after = c >= 0 && Prog::IsWordChar(static_cast<uint8_t>(c));
  flags |= before != after ? kEmptyWordBoundary : kEmptyNonWordBoundary;
  return flags;
}

uint8_t TDFA::ContextAfter(int c) const {
  if (!has_empty_)
    return 0;
  uint8_t ctx = 0;
  if (c == '\n')
    ctx |= kCtxNewline;
  if (Prog::IsWordChar(static_cast<uint8_t>(c)))
    ctx |= kCtxWord;
  return ctx;
}

void TDFA::Expand(int id0, int16_t* slots, uint32_t flags, int c,
                  Expansion* x) {
  stack_.clear();
  stack_.push_back({id0, -1, 0});
  while (!stack_.empty()) {
    StackEntry e = stack_.back();
    stack_.pop_back();
    if (e.slot >= 0) {
      slots[e.slot] = e.old;
      continue;
    }
    int id = e.id;
    while (id != 0 && !visited_.contains(id)) {
      visited_.insert_new(id);
      Prog::Inst* ip = prog_->inst(id);
      switch (ip->opcode()) {
        case kInstFail:
          id = 0;
          break;
        case kInstAltMatch:
          id = id + 1;
          break;
        case kInstNop:
          if (!ip->last())
            stack_.push_back({id + 1, -1, 0});
          id = ip->out();
          break;
        case kInstCapture: {
          if (!ip->last())
            stack_.push_back({id + 1, -1, 0});
          int cap = ip->cap();
          int slot = cap == 0 ? 0 : cap - 1;
          if (cap != 1 && slot < nslots_) {
            stack_.push_back({0, slot, slots[slot]});
            slots[slot] = kNow;
          }
          id = ip->out();
          break;
        }
        case kInstEmptyWidth:
          if (!ip->last())
            stack_.push_back({id + 1, -1, 0});
          id = (ip->empty() & ~flags) ? 0 : ip->out();
          break;
        case kInstByteRange:
          if (c < 0 || !ip->Matches(c)) {
            id = ip->last() ? 0 : id + 1;
            break;
          }
          x->roots.push_back(ip->out());
          x->slots.insert(x->slots.end(), slots, slots + nslots_);
          id = ip->hint() == 0 ? 0 : id + ip->hint();
          break;
        case kInstMatch:
          if (full_ && c >= 0) {
            id = ip->last() ? 0 : id + 1;
            break;
          }
          if (!x->match) {
            x->match = true;
            x->match_slots.assign(slots, slots + nslots_);
            if (!full_) {
              x->cut = true;
              return;
            }
          }
          id = ip->last() ? 0 : id + 1;
          break;
        default:
          id = 0;
          break;
      }
    }
  }
}

bool TDFA::Charge(int64_t cost) {
  if (budget_->fetch_sub(cost, std::memory_order_relaxed) < cost) {
    budget_->fetch_add(cost, std::memory_order_relaxed);
    return false;
  }
  return true;
}

uintptr_t TDFA::Publish(const Expansion& x, uint8_t inject, uint8_t ctx,
                        int src_nregs, bool final) {
  std::vector<int16_t> src;
  TState* to = NULL;
  if (!final) {
    std::vector<int16_t> map_src(src_nregs, kUnset);
    int16_t map_now = kUnset;
    std::vector<int16_t> regs(x.slots.size());
    for (size_t i = 0; i < regs.size(); i++) {
      int16_t v = x.slots[i];
      if (v == kUnset) {
        regs[i] = kUnset;
        continue;
      }
      int16_t& mapped = v == kNow ? map_now : map_src[v];
      if (mapped == kUnset) {
        if (src.size() >=
            static_cast<size_t>(std::numeric_limits<int16_t>::max()))
          return 0;
        mapped = static_cast<int16_t>(src.size());
        src.push_back(v);
      }
      regs[i] = mapped;
    }

    std::string key;
    key.push_back(static_cast<char>(inject));
    key.push_back(static_cast<char>(ctx));
    key.append(reinterpret_cast<const char*>(x.roots.data()),
               x.roots.size() * sizeof(int));
    key.append(reinterpret_cast<const char*>(regs.data()),
               regs.size() * sizeof(int16_t));

    TState*& slot = states_[key];
    if (slot == NULL) {
      int64_t cost = static_cast<int64_t>(sizeof(TState) + key.size() * 2 +
                                          (2 * nclass_ + 1) * sizeof(uintptr_t));
      if (!Charge(cost)) {
        states_.erase(key);
        return 0;
      }
      size_t ntrans = 2 * static_cast<size_t>(nclass_) + 1;
      std::unique_ptr<TState, StateDeleter> s(new (::operator new(
          sizeof(TState) + ntrans * sizeof(std::atomic<uintptr_t>))) TState);
      for (size_t i = 0; i < ntrans; i++)
        new (&s->next()[i]) std::atomic<uintptr_t>(kUnbuilt);
      s->roots = x.roots;
      s->slots = std::move(regs);
      s->nregs = static_cast<int>(src.size());
      s->inject = inject;
      s->ctx = ctx;
      s->dead = s->roots.empty() && inject == kInjectNone;
      s->idle = s->roots.empty() && inject == kInjectAlways;
      slot = s.get();
      owned_states_.push_back(std::move(s));
    }
    to = slot;

    bool identity = !x.match && static_cast<int>(src.size()) == src_nregs;
    for (size_t r = 0; identity && r < src.size(); r++)
      identity = src[r] == static_cast<int16_t>(r);
    if (identity)
      return reinterpret_cast<uintptr_t>(to) |
             (to->dead || to->idle ? kSpecialTag : 0);
  }
  size_t nslots = x.match ? x.match_slots.size() : 0;
  size_t size = sizeof(TEdge) + (src.size() + nslots) * sizeof(int16_t);
  if (!Charge(static_cast<int64_t>(size)))
    return 0;
  std::unique_ptr<TEdge, EdgeDeleter> edge(
      new (::operator new(size)) TEdge);
  edge->to = to;
  edge->nregs = static_cast<int>(src.size());
  edge->match = x.match;
  std::copy(src.begin(), src.end(), edge->src());
  std::copy(x.match_slots.begin(), x.match_slots.begin() + nslots,
            edge->src() + src.size());
  uintptr_t tagged = reinterpret_cast<uintptr_t>(edge.get()) | kEdgeTag |
                     (to != NULL && (to->dead || to->idle) ? kSpecialTag : 0);
  owned_edges_.push_back(std::move(edge));
  return tagged;
}

uintptr_t TDFA::Start(uint8_t inject, uint8_t ctx) {
  std::atomic<uintptr_t>& start = start_[inject][ctx];
  uintptr_t cur = start.load(std::memory_order_acquire);
  if (cur != 0)
    return cur;
  absl::MutexLock lock(&mu_);
  cur = start.load(std::memory_order_relaxed);
  if (cur != 0)
    return cur;
  Expansion x;
  cur = Publish(x, inject, ctx, 0, false);
  if (cur == 0) {
    failed_.store(true, std::memory_order_relaxed);
    return 0;
  }
  start.store(cur, std::memory_order_release);
  return cur;
}

uintptr_t TDFA::Next(TState* s, int index, int c) {
  absl::MutexLock lock(&mu_);
  return NextLocked(s, index, c);
}

void TDFA::ComputeRun(TState* s) {
  s->run.store(kRunNone, std::memory_order_relaxed);
  for (int cls = 0; cls < nclass_; cls++) {
    if (NextLocked(s, cls, class_byte_[cls]) == 0)
      return;
  }
  uint64_t loop[4] = {};
  uint64_t exit[4] = {};
  const uint8_t* bytemap = prog_->bytemap();
  for (int b = 0; b < 256; b++) {
    uintptr_t next = s->next()[bytemap[b]].load(std::memory_order_relaxed);
    bool loops = (next & kTagMask) == 0 && Untag(next) == s;
    (loops ? loop : exit)[b >> 6] |= uint64_t{1} << (b & 63);
  }
  int nloop = absl::popcount(loop[0]) + absl::popcount(loop[1]);
  int nexit = absl::popcount(exit[0]) + absl::popcount(exit[1]);
  uint8_t run = kRunNone;
  if (nloop <= nexit)
    run = kRunNone;
  else if ((exit[0] | exit[1] | exit[2] | exit[3]) == 0)
    run = kRunToEnd;
  else if (s->run_finder.Build(exit))
    run = kRunFind;
  else if (s->run_finder.Build(loop))
    run = kRunSkip;
  s->run.store(run, std::memory_order_release);
  if (run == kRunNone)
    return;
  uintptr_t tagged = reinterpret_cast<uintptr_t>(s) | kRunTag;
  for (int cls = 0; cls < nclass_; cls++) {
    if (s->next()[cls].load(std::memory_order_relaxed) ==
        reinterpret_cast<uintptr_t>(s))
      s->next()[cls].store(tagged, std::memory_order_release);
  }
}

uintptr_t TDFA::NextLocked(TState* s, int index, int c) {
  uintptr_t next = s->next()[index].load(std::memory_order_relaxed);
  if (next != kUnbuilt)
    return next;
  uint32_t flags = Flags(s->ctx, c);
  bool final = index >= nclass_;
  Expansion x;
  std::vector<int16_t> slots(nslots_);
  visited_.clear();
  for (size_t i = 0; i < s->roots.size() && !x.cut; i++) {
    memcpy(slots.data(), &s->slots[i * nslots_], nslots_ * sizeof(int16_t));
    Expand(s->roots[i], slots.data(), flags, final ? -1 : c, &x);
  }
  if (!x.cut && s->inject != kInjectNone) {
    std::fill(slots.begin(), slots.end(), kUnset);
    slots[0] = kNow;
    Expand(prog_->start(), slots.data(), flags, final ? -1 : c, &x);
  }
  uint8_t inject = s->inject == kInjectAlways ? kInjectAlways : kInjectNone;
  if (x.match && !full_)
    inject = kInjectNone;
  next = Publish(x, inject, final ? 0 : ContextAfter(c), s->nregs, final);
  if (next == 0) {
    failed_.store(true, std::memory_order_relaxed);
    return 0;
  }
  s->next()[index].store(next, std::memory_order_release);
  if (next == reinterpret_cast<uintptr_t>(s) &&
      s->run.load(std::memory_order_relaxed) == kRunUnknown) {
    ComputeRun(s);
    next = s->next()[index].load(std::memory_order_relaxed);
  }
  return next;
}

void TDFA::Record(const int16_t* slots, const char* const* regs,
                  const char* p, absl::string_view* match, int nmatch) const {
  auto at = [&](int16_t v) -> const char* {
    if (v == kNow)
      return p;
    if (v == kUnset)
      return NULL;
    return regs[v];
  };
  const char* begin = at(slots[0]);
  match[0] = absl::string_view(begin, p - begin);
  for (int i = 1; i < nmatch; i++) {
    const char* b = at(slots[2 * i - 1]);
    const char* e = at(slots[2 * i]);
    if (b == NULL || e == NULL) {
      match[i] = absl::string_view();
      continue;
    }
    match[i] = absl::string_view(b, e - b);
  }
}

int TDFA::Search(absl::string_view text, absl::string_view context,
                 bool anchored, bool earliest, absl::string_view* match,
                 int nmatch) {
  if (failed_.load(std::memory_order_relaxed))
    return -1;
  const char* p = text.data();
  const char* end = p + text.size();
  uint8_t ctx = 0;
  if (has_empty_) {
    if (p == context.data())
      ctx = kCtxBegin;
    else
      ctx = ContextAfter(static_cast<uint8_t>(p[-1]));
  }
  uintptr_t cur = Start(anchored ? kInjectOnce : kInjectAlways, ctx);
  if (cur == 0)
    return -1;
  TState* s = Untag(cur);
  const char* stack_regs[64];
  std::vector<const char*> heap_regs;
  size_t capacity = std::size(stack_regs) / 2;
  const char** regs = stack_regs;
  const char** spare = stack_regs + capacity;
  const uint8_t* bytemap = prog_->bytemap();
  const bool accel = prog_->can_prefix_accel();
  bool matched = false;
  while (!s->dead && p != end) {
    if (accel && s->idle) {
      const char* q =
          reinterpret_cast<const char*>(prog_->PrefixAccel(p, end - p));
      if (q == NULL)
        q = end;
      if (q != p) {
        p = q;
        uint8_t skipped_ctx = 0;
        if (has_empty_)
          skipped_ctx = ContextAfter(static_cast<uint8_t>(p[-1]));
        uintptr_t idle = Start(kInjectAlways, skipped_ctx);
        if (idle == 0)
          return -1;
        s = Untag(idle);
        if (p == end)
          break;
      }
    }
    while (p != end) {
      uintptr_t next;
      s = StepPlain(s, &p, end, bytemap, &next);
      if (p == end)
        break;
      int c = static_cast<uint8_t>(*p);
      int cls = bytemap[c];
      if (next == kUnbuilt) {
        next = Next(s, cls, c);
        if (next == 0)
          return -1;
      }
      if (next & kEdgeTag) {
        const TEdge* e = UntagEdge(next);
        if (e->match) {
          Record(e->match_slots(), regs, p, match, nmatch);
          if (earliest)
            return 1;
          matched = true;
        }
        size_t nregs = static_cast<size_t>(e->nregs);
        if (nregs > capacity) {
          std::vector<const char*> grown(2 * nregs);
          std::copy(regs, regs + capacity, grown.begin());
          heap_regs.swap(grown);
          capacity = nregs;
          regs = heap_regs.data();
          spare = heap_regs.data() + nregs;
        }
        const int16_t* src = e->src();
        for (size_t r = 0; r < nregs; r++)
          spare[r] = src[r] == kNow ? p : regs[src[r]];
        std::swap(regs, spare);
        s = e->to;
      } else {
        s = Untag(next);
      }
      ++p;
      if (next & kSpecialTag)
        break;
    }
  }
  if (!s->dead) {
    int c = end < context.data() + context.size() ? static_cast<uint8_t>(*end)
                                                   : -1;
    int index = c < 0 ? 2 * nclass_ : nclass_ + bytemap[c];
    uintptr_t next = s->next()[index].load(std::memory_order_acquire);
    if (next == kUnbuilt) {
      next = Next(s, index, c);
      if (next == 0)
        return -1;
    }
    const TEdge* e = UntagEdge(next);
    if (e->match) {
      Record(e->match_slots(), regs, p, match, nmatch);
      matched = true;
    }
  }
  return matched ? 1 : 0;
}

void Prog::ReserveTDFA() {
  if (tdfa_budget_.load(std::memory_order_relaxed) >= 0)
    return;
  int64_t reserved = dfa_mem_ / 4;
  dfa_mem_ -= reserved;
  tdfa_budget_.store(reserved, std::memory_order_relaxed);
}

void Prog::DeleteTDFA() {
  if (tdfa_ == NULL)
    return;
  for (int i = 0; i < tdfa_count_; i++)
    delete tdfa_[i].load(std::memory_order_relaxed);
}

int Prog::SearchTDFA(absl::string_view text, absl::string_view context,
                     Anchor anchor, MatchKind kind, absl::string_view* match,
                     int nmatch) {
  if (kind == kLongestMatch || kind == kManyMatch ||
      tdfa_budget_.load(std::memory_order_relaxed) < 0)
    return -1;
  if (context.data() == NULL)
    context = text;
  if (anchor_start() && BeginPtr(context) != BeginPtr(text))
    return 0;
  if (anchor_end() && EndPtr(context) != EndPtr(text))
    return 0;
  bool anchored = anchor == kAnchored || anchor_start() || kind == kFullMatch;
  bool full = kind == kFullMatch || anchor_end();
  int ngroups = inst_count(kInstCapture) / 2;
  absl::call_once(tdfa_once_, [this, ngroups] {
    tdfa_count_ = 2 * (ngroups + 1);
    tdfa_.reset(new std::atomic<TDFA*>[tdfa_count_]);
    for (int i = 0; i < tdfa_count_; i++)
      tdfa_[i].store(NULL, std::memory_order_relaxed);
  });
  int tracked = std::max(1, std::min(nmatch, ngroups + 1));
  std::atomic<TDFA*>& slot = tdfa_[2 * (tracked - 1) + full];
  TDFA* tdfa = slot.load(std::memory_order_acquire);
  if (tdfa == NULL) {
    auto created =
        std::make_unique<TDFA>(this, 2 * tracked - 1, full, &tdfa_budget_);
    if (slot.compare_exchange_strong(tdfa, created.get(),
                                     std::memory_order_acq_rel))
      tdfa = created.release();
  }
  std::vector<absl::string_view> scratch;
  absl::string_view* out = match;
  if (nmatch < tracked) {
    scratch.resize(tracked);
    out = scratch.data();
  }
  int found = tdfa->Search(text, context, anchored, nmatch == 0 && !full, out,
                           tracked);
  if (found == 1) {
    if (out != match) {
      for (int i = 0; i < nmatch; i++)
        match[i] = out[i];
    }
    for (int i = tracked; i < nmatch; i++)
      match[i] = absl::string_view();
  }
  return found;
}

}  // namespace re2
