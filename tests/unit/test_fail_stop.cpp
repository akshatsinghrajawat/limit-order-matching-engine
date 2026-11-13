// Test 29: what happens when an allocation fails inside the book (ADR-012).
//
// Prevention: with the caller's result reserved at startup, no mutating
// entry point allocates, so none can fail. Recovery: if one did fail it
// would terminate, the journal already holds the command, and replaying the
// journal gives the state an uninterrupted run reaches.
//
// This file replaces global operator new to count and to fail allocations,
// so it does not link alloc_counter. CHECK instead of assert so a Release
// build (-DNDEBUG) still verifies.
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "command_stream.hpp"
#include "engine/book.hpp"
#include "engine/replay.hpp"
#include "journal_loop.hpp"

#if defined(__unix__) || defined(__APPLE__)
#define HAVE_FORK 1
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

namespace {

bool g_armed = false;
bool g_throw = false;
std::size_t g_allocations = 0;
std::size_t g_fail_at = 0;      // fail the allocation with this number, counting from 1; 0 for none
std::size_t g_failed_size = 0;  // the size that allocation asked for

void* allocate(std::size_t n) {
  if (g_armed) {
    ++g_allocations;
    if (g_throw || g_allocations == g_fail_at) {
      g_failed_size = n;
      throw std::bad_alloc();
    }
  }
  void* p = std::malloc(n != 0 ? n : 1);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

// Counts allocations while in scope, and fails them when asked to.
struct Armed {
  explicit Armed(bool fail) {
    g_allocations = 0;
    g_throw = fail;
    g_armed = true;
  }
  ~Armed() { g_armed = false; }
  std::size_t allocations() const { return g_allocations; }
};

// Lets the first n - 1 allocations through and fails the nth.
struct FailNth {
  explicit FailNth(std::size_t n) {
    g_allocations = 0;
    g_throw = false;
    g_fail_at = n;
    g_failed_size = 0;
    g_armed = true;
  }
  ~FailNth() {
    g_armed = false;
    g_fail_at = 0;
  }
};

}  // namespace

void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace engine;
using journal::AppendStatus;

namespace {

constexpr Index kCapacity = 64;

// The six entry points that change the book. A throw out of any of them
// would leave a command half applied, so none may throw (ADR-012).
using std::declval;
static_assert(noexcept(declval<Book&>().add_limit_order(declval<Book::AddResult&>(), OrderId{}, Side{}, Price{}, Qty{},
                                                        std::uint32_t{}, TimeInForce{})));
static_assert(noexcept(declval<Book&>().add_limit_order(OrderId{}, Side{}, Price{}, Qty{}, std::uint32_t{},
                                                        TimeInForce{})));
static_assert(noexcept(declval<Book&>().add_market_order(declval<Book::AddResult&>(), OrderId{}, Side{}, Qty{},
                                                         std::uint32_t{})));
static_assert(noexcept(declval<Book&>().add_market_order(OrderId{}, Side{}, Qty{}, std::uint32_t{})));
static_assert(noexcept(declval<Book&>().modify_order(OrderId{}, Price{}, Qty{})));
static_assert(noexcept(declval<Book&>().cancel_order(OrderId{})));

constexpr std::uint32_t kMaker = 1, kOtherMaker = 2, kTaker = 3;

// Rests n sells at 100, 101, ... and n buys at 99, 98, ... so a sweep of
// either side crosses n levels.
void fill_both_sides(Book& book, Book::AddResult& scratch, OrderId first_id, int n) {
  for (int i = 0; i < n; ++i) {
    book.add_limit_order(scratch, first_id + 2 * i, Side::Sell, 100 + i, 1, kMaker);
    book.add_limit_order(scratch, first_id + 2 * i + 1, Side::Buy, 99 - i, 1, kOtherMaker);
  }
}

// Every trade the test sees, in order, so the ids can be checked at the end.
struct Ids {
  std::uint64_t expected = 0;
  void take(const Book::AddResult& r) {
    for (const Trade& t : r.trades) CHECK(t.trade_id == expected++);
  }
};

// The injector has to see allocations at all, or "zero" below proves nothing.
void the_injector_sees_the_allocations_a_reserve_prevents() {
  Book book(kCapacity);
  Book::AddResult unreserved;
  fill_both_sides(book, unreserved, 1000, 20);
  unreserved.trades = std::vector<Trade>();
  Armed armed(false);
  book.add_market_order(unreserved, 1, Side::Buy, 20, kTaker);
  CHECK(unreserved.trades.size() == 20);
  CHECK(armed.allocations() > 0);
}

void no_entry_point_allocates_once_the_result_is_reserved() {
  Book book(kCapacity);
  Book::AddResult scratch;
  scratch.trades.reserve(kCapacity);
  const std::size_t reserved = scratch.trades.capacity();
  Ids ids;
  OrderId next_id = 1;

  {  // limit sweeps of each side, and a market sweep of each side
    fill_both_sides(book, scratch, next_id, 25);
    next_id += 50;
    Armed armed(true);
    book.add_limit_order(scratch, next_id++, Side::Buy, 200, 25, kTaker);
    CHECK(scratch.trades.size() == 25);
    ids.take(scratch);
    book.add_limit_order(scratch, next_id++, Side::Sell, 1, 25, kTaker);
    CHECK(scratch.trades.size() == 25);
    ids.take(scratch);
    CHECK(armed.allocations() == 0);
  }
  {
    Armed armed(true);
    fill_both_sides(book, scratch, next_id, 25);
    next_id += 50;
    book.add_market_order(scratch, next_id++, Side::Buy, 100, kTaker);
    CHECK(scratch.trades.size() == 25);
    ids.take(scratch);
    book.add_market_order(scratch, next_id++, Side::Sell, 100, kTaker);
    CHECK(scratch.trades.size() == 25);
    ids.take(scratch);
    CHECK(armed.allocations() == 0);
  }

  {  // the largest sweep there can be: every slot in the pool is a resting order
    Book full(kCapacity);
    Book::AddResult r;
    r.trades.reserve(kCapacity);
    for (Index i = 0; i < kCapacity; ++i) full.add_limit_order(r, 1 + i, Side::Sell, 100 + i, 1, kMaker);
    Armed armed(true);
    full.add_market_order(r, 5000, Side::Buy, kCapacity + 10, kTaker);
    CHECK(r.trades.size() == kCapacity);
    CHECK(r.trades.capacity() == kCapacity);
    CHECK(armed.allocations() == 0);
  }

  {  // a self-match in the middle of a sweep: ten fills, then the sweep stops
    Book b(kCapacity);
    Book::AddResult r;
    r.trades.reserve(kCapacity);
    for (int i = 0; i < 10; ++i) b.add_limit_order(r, 1 + i, Side::Sell, 100 + i, 1, kMaker);
    b.add_limit_order(r, 50, Side::Sell, 110, 1, kTaker);
    for (int i = 0; i < 10; ++i) b.add_limit_order(r, 60 + i, Side::Sell, 111 + i, 1, kMaker);
    Armed armed(true);
    b.add_limit_order(r, 99, Side::Buy, 200, 30, kTaker);
    CHECK(r.stp_rejected && r.trades.size() == 10);
    CHECK(armed.allocations() == 0);
  }

  {  // fill-or-kill, immediate-or-cancel, modify, cancel
    Book b(kCapacity);
    Book::AddResult r;
    r.trades.reserve(kCapacity);
    fill_both_sides(b, r, 1, 20);
    Armed armed(true);
    b.add_limit_order(r, 500, Side::Buy, 200, 10, kTaker, TimeInForce::FOK);
    CHECK(r.trades.size() == 10);
    b.add_limit_order(r, 501, Side::Buy, 200, 100, kTaker, TimeInForce::IOC);
    CHECK(r.trades.size() == 10);
    CHECK(b.modify_order(2, 50, 3));
    CHECK(b.cancel_order(4));
    CHECK(armed.allocations() == 0);
  }

  CHECK(scratch.trades.capacity() == reserved);
  CHECK(ids.expected == 100);
}

// The header's capacity is the file's word. Over the limit, replay says so
// before it allocates anything: every allocation is made to fail here, so a
// book built for 4 billion slots would show as an OutOfMemory, not a refusal.
void a_capacity_far_over_the_limit_allocates_nothing() {
  const replay::ReplayLimits limits;
  const std::vector<Command> cmds = stream::make(3, 50);
  for (const Index capacity : {limits.max_pool_capacity + 1, std::numeric_limits<Index>::max() - 1,
                               std::numeric_limits<Index>::max()}) {
    const auto bytes = stream::journal_bytes(capacity, cmds);
    replay::Replayed r;
    Armed armed(true);
    CHECK(!replay::replay(bytes.data(), bytes.size(), r));
    CHECK(armed.allocations() == 0);
    CHECK(r.limit == replay::Limit::PoolCapacity);
    CHECK(r.commands == 0);
  }
}

// A failed allocation while building the book is reported, at every point
// it can happen. The last allocation of the build is the trade buffer, which
// is reserved to the pool capacity (ADR-012).
void a_failed_allocation_while_building_is_reported_as_out_of_memory() {
  const auto bytes = stream::journal_bytes(kCapacity, {});

  std::size_t build = 0;
  {
    Armed armed(false);
    Book book(kCapacity);
    Book::AddResult scratch;
    scratch.trades.reserve(kCapacity);
    build = armed.allocations();
  }
  CHECK(build > 1);

  for (std::size_t k = 1; k <= build; ++k) {
    replay::Replayed r;
    {
      FailNth fail(k);
      CHECK(!replay::replay(bytes.data(), bytes.size(), r));
      if (k == build) CHECK(g_failed_size == kCapacity * sizeof(Trade));
    }
    CHECK(r.limit == replay::Limit::OutOfMemory);
    CHECK(r.commands == 0 && r.log.empty() && r.book.empty());
  }

  replay::Replayed ok;
  CHECK(replay::replay(bytes.data(), bytes.size(), ok));
  CHECK(ok.limit == replay::Limit::None);
}

// The journal-first loop, stopped after each command in turn: replaying what
// the journal holds gives the same book and the same log as the loop, and
// the numbers the next commands receive are the same too. Two commands
// that trade follow, so a trade id or a seq_no that had been lost shows in
// their lines.
void replaying_the_journal_after_any_stop_matches_the_loop() {
  const char* path = "fail_stop_loop.bin";
  constexpr Index kSmall = 16;
  std::vector<Command> cmds = stream::make(11, 120);
  cmds.push_back(limit_command(900001, Side::Sell, 1, 1, 7));
  cmds.push_back(limit_command(900002, Side::Buy, 1000000, 3, 8));

  for (std::size_t k = 0; k <= 120; ++k) {
    std::vector<Command> run(cmds.begin(), cmds.begin() + static_cast<std::ptrdiff_t>(k));
    run.push_back(cmds[120]);
    run.push_back(cmds[121]);

    std::string log;
    std::string book_text;
    {
      loop::Loop l(path, kSmall);
      CHECK(l.opened());
      for (std::size_t i = 0; i < run.size(); ++i) {
        CHECK(l.step(run[i]) == AppendStatus::Ok);
        replay::append_events(log, i, run[i], l.outcome(), l.scratch().trades);
      }
      book_text = replay::serialize_book(l.book());
    }

    std::vector<std::uint8_t> bytes;
    CHECK(journal::load(path, bytes));
    replay::Replayed rebuilt;
    CHECK(replay::replay(bytes.data(), bytes.size(), rebuilt));
    CHECK(rebuilt.stop == journal::Status::End);
    CHECK(rebuilt.commands == run.size());
    CHECK(rebuilt.log == log);
    CHECK(rebuilt.book == book_text);
  }
  std::remove(path);
}

#ifdef HAVE_FORK
// A process that fails an allocation inside apply() is gone. Rather than
// wait for a real one, the child empties the result so its next trade has
// to grow it, then makes that allocation fail. The journal write is outside
// the failing region, so the record is in the file when the child dies.
void a_failed_allocation_ends_the_process_and_the_journal_replays() {
  const char* path = "fail_stop_death.bin";
  const std::vector<Command> cmds = {
      limit_command(1, Side::Sell, 100, 5, 1),
      limit_command(2, Side::Sell, 101, 5, 1),
      limit_command(3, Side::Buy, 101, 10, 2),  // two trades
  };

  std::fflush(nullptr);
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    const int quiet = open("/dev/null", O_WRONLY);
    if (quiet >= 0) dup2(quiet, 2);
    loop::Loop l(path, 16);
    if (!l.opened()) _exit(2);
    for (std::size_t i = 0; i < 2; ++i) {
      if (l.step(cmds[i]) != AppendStatus::Ok) _exit(3);
    }
    l.scratch().trades = std::vector<Trade>();
    if (l.journal(cmds[2]) != AppendStatus::Ok) _exit(4);
    Armed armed(true);
    l.apply(cmds[2]);
    _exit(0);  // not reached: the first trade allocates, the allocation throws, noexcept terminates
  }

  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

  std::vector<std::uint8_t> bytes;
  CHECK(journal::load(path, bytes));
  replay::Replayed rebuilt;
  CHECK(replay::replay(bytes.data(), bytes.size(), rebuilt));
  CHECK(rebuilt.commands == 3 && rebuilt.stop == journal::Status::End);

  Book book(16);
  Book::AddResult scratch;
  std::string log;
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const replay::Outcome o = replay::apply(book, cmds[i], scratch);
    replay::append_events(log, i, cmds[i], o, scratch.trades);
  }
  CHECK(rebuilt.log == log);
  CHECK(rebuilt.book == replay::serialize_book(book));
  CHECK(log.find("trade seq=2 id=0 ") != std::string::npos);
  CHECK(log.find("trade seq=2 id=1 ") != std::string::npos);
  std::remove(path);
}
#endif

}  // namespace

int main() {
  the_injector_sees_the_allocations_a_reserve_prevents();
  no_entry_point_allocates_once_the_result_is_reserved();
  replaying_the_journal_after_any_stop_matches_the_loop();
  a_capacity_far_over_the_limit_allocates_nothing();
  a_failed_allocation_while_building_is_reported_as_out_of_memory();
#ifdef HAVE_FORK
  a_failed_allocation_ends_the_process_and_the_journal_replays();
#else
  std::puts("fail-stop death test skipped: no fork");
#endif
  std::puts("fail stop ok");
}
