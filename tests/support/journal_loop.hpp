#pragma once

// The ingest loop ADR-012 describes, in the smallest form that is correct:
// reserve once, then for each command write it to the journal and apply it
// only if the write succeeded. The book is never ahead of the journal, so
// replaying the journal after a stop rebuilds the book exactly.
//
// journal() and apply() are separate so a test can stop between them, which
// is the point a real process is killed at.
#include <vector>

#include "engine/book.hpp"
#include "engine/command.hpp"
#include "engine/journal.hpp"
#include "engine/replay.hpp"

namespace loop {

class Loop {
 public:
  Loop(const char* path, engine::Index capacity) : book_(capacity) {
    opened_ = writer_.open(path, capacity);
    scratch_.trades.reserve(capacity);  // a sweep makes at most one trade per resting order
  }

  bool opened() const { return opened_; }

  engine::journal::AppendStatus journal(const engine::Command& c) { return writer_.append(c); }

  void apply(const engine::Command& c) { outcome_ = engine::replay::apply(book_, c, scratch_); }

  engine::journal::AppendStatus step(const engine::Command& c) {
    const engine::journal::AppendStatus status = journal(c);
    if (status == engine::journal::AppendStatus::Ok) apply(c);
    return status;
  }

  const engine::Book& book() const { return book_; }
  engine::Book::AddResult& scratch() { return scratch_; }
  const engine::replay::Outcome& outcome() const { return outcome_; }

 private:
  engine::Book book_;
  engine::Book::AddResult scratch_;
  engine::replay::Outcome outcome_;
  engine::journal::Writer writer_;
  bool opened_ = false;
};

}  // namespace loop
