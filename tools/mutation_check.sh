#!/usr/bin/env bash
# Checks that the test suite notices when include/engine/book.hpp,
# include/engine/id_map.hpp, include/engine/price_levels.hpp,
# include/engine/types.hpp, include/engine/command.hpp,
# include/engine/journal.hpp or include/engine/replay.hpp is wrong.
#
# Each mutant below makes one small change to a copy of one header, rebuilds,
# and runs ctest. A mutant is killed if the build succeeds and some test
# fails. A survivor means either a gap in the tests or an equivalent
# mutant; both need a look. The working tree is never modified.
#
# Builds are Debug because test_basic.cpp uses assert(), which -DNDEBUG
# removes. Sanitizers follow the project default (on); set
# ENABLE_SANITIZERS=OFF where the toolchain can't link them (MSYS2).
#
# Exit status: 0 all killed, 1 a mutant survived, 2 the check itself broke
# (baseline failing, pattern not found, mutant that doesn't compile).

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

src=$work/src
build=$work/build

mkdir "$src"
cp -r "$root/include" "$root/tests" "$root/bench" "$root/CMakeLists.txt" "$src/"

# Selects the header the following mutants apply to. mutate() reads the
# untouched copy and writes the mutated one into the source tree.
use_target() {
  header=$1
  pristine=$work/$(basename "$1").orig
  [ -e "$pristine" ] || cp "$src/$header" "$pristine"
}
use_target include/engine/book.hpp

cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_SANITIZERS="${ENABLE_SANITIZERS:-ON}" > "$work/configure.log" 2>&1 || {
  cat "$work/configure.log"
  exit 2
}

# Prints the names of failing tests, one per line. Empty means all passed.
# A build failure is reported on stdout as BUILD_FAILED.
run_suite() {
  if ! cmake --build "$build" --parallel > "$work/build.log" 2>&1; then
    echo BUILD_FAILED
    return
  fi
  if ctest --test-dir "$build" --timeout 120 > "$work/ctest.log" 2>&1; then
    return
  fi
  local names
  names=$(sed -n 's/^[[:space:]]*[0-9][0-9]* - \([A-Za-z0-9_]*\) .*/\1/p' "$work/ctest.log")
  echo "${names:-ctest_error}"
}

# Replaces the Nth occurrence of a literal string in the header. Literal,
# not regex, so the C++ in the patterns needs no escaping.
mutate() {
  awk -v old="$2" -v new="$3" -v want="$1" '
    {
      line = $0
      out = ""
      while ((i = index(line, old)) > 0) {
        if (++seen == want) {
          out = out substr(line, 1, i - 1) new
          line = substr(line, i + length(old))
          hit = 1
          break
        }
        out = out substr(line, 1, i - 1 + length(old))
        line = substr(line, i + length(old))
      }
      print out line
    }
    END { exit hit ? 0 : 3 }
  ' "$pristine" > "$src/$header"
}

echo "baseline"
baseline=$(run_suite)
if [ -n "$baseline" ]; then
  echo "  baseline does not pass: $baseline"
  tail -n 40 "$work/build.log" "$work/ctest.log"
  exit 2
fi
echo "  all tests pass"
echo

total=0
survivors=()
broken=0

check() {
  local name=$1 nth=$2 old=$3 new=$4
  total=$((total + 1))

  if ! mutate "$nth" "$old" "$new"; then
    printf '  %-34s PATTERN NOT FOUND\n' "$name"
    broken=1
    return
  fi
  local failed
  failed=$(run_suite)
  cp "$pristine" "$src/$header"
  if [ "$failed" = BUILD_FAILED ]; then
    printf '  %-34s DOES NOT COMPILE\n' "$name"
    broken=1
  elif [ -z "$failed" ]; then
    printf '  %-34s SURVIVED\n' "$name"
    survivors+=("$name")
  else
    printf '  %-34s killed by %s\n' "$name" "$(echo $failed | tr ' ' ',')"
  fi
}

# S3: self-trade prevention
check stp-ignored-in-cross 1 \
  'if (maker.account == incoming.account) return true;' \
  'if (false) return true;'
check stp-ignored-in-fok-precheck 1 \
  'if (order.account == account) return total;' \
  'if (false) return total;'
check stp-residual-still-rests 1 \
  '(tif == TimeInForce::GTC) && !result.stp_rejected;' \
  '(tif == TimeInForce::GTC);'

# S5: crossing rules and time in force
check trade-at-taker-price 1 \
  'incoming.order_id, maker.price,' \
  'incoming.order_id, incoming.price,'
check buy-cross-strict 2 \
  '{ return resting <= taker; }' \
  '{ return resting < taker; }'
check sell-cross-strict 2 \
  '{ return resting >= taker; }' \
  '{ return resting > taker; }'
check fok-precheck-buy-strict 1 \
  '{ return resting <= taker; }' \
  '{ return resting < taker; }'
check fok-precheck-off-by-one 1 \
  'if (available < static_cast<std::uint64_t>(qty))' \
  'if (available <= static_cast<std::uint64_t>(qty))'
check fok-precheck-sum-narrowed 1 \
  'std::uint64_t total = 0;' \
  'Qty total = 0;'
check ioc-rests-residual 1 \
  '(tif == TimeInForce::GTC) && !result.stp_rejected;' \
  '(tif != TimeInForce::FOK) && !result.stp_rejected;'
check market-never-crosses 1 \
  '[](Price, Price) { return true; }' \
  '[](Price, Price) { return false; }'
check duplicate-id-accepted 1 \
  'if (qty == 0 || id_map_.contains(order_id)) return;' \
  'if (qty == 0) return;'

# S2/S6: modify and priority
check modify-equal-qty-loses-priority 1 \
  'new_qty <= order->qty' \
  'new_qty < order->qty'
check modify-increase-keeps-priority 1 \
  'new_qty <= order->qty' \
  'true'
check modify-seq-no-not-advanced 1 \
  'order->seq_no = seq;' \
  ';'
check modify-crosses-allowed 1 \
  'if (would_cross(order->side, new_price)) return false;' \
  ';'
check modify-buy-touch-allowed 1 \
  'price >= asks_.best().price' \
  'price > asks_.best().price'
check modify-sell-touch-allowed 1 \
  'price <= bids_.best().price' \
  'price < bids_.best().price'

# ADR-009: every command takes a seq_no, and trades and resting orders carry it
check limit-takes-no-seq 1 \
  'const SeqNo seq = next_seq_++;' \
  'const SeqNo seq = next_seq_;'
check market-takes-no-seq 2 \
  'const SeqNo seq = next_seq_++;' \
  'const SeqNo seq = next_seq_;'
check modify-takes-no-seq 3 \
  'const SeqNo seq = next_seq_++;' \
  'const SeqNo seq = next_seq_;'
check cancel-takes-no-seq 1 \
  '++next_seq_;' \
  ';'
check trade-seq-zero 1 \
  'fill, next_trade_id_, seq});' \
  'fill, next_trade_id_, 0});'
check trade-id-not-advanced 1 \
  '++next_trade_id_;' \
  ';'
check rest-seq-zero 1 \
  'pool_.get(h)->seq_no = seq;' \
  'pool_.get(h)->seq_no = 0;'

# Bookkeeping: levels, pool slots and the id map
check modify-leaves-empty-level 1 \
  'if (old_level.head == kNilIndex) erase_level(order->side, order->price);' \
  ';'
check cancel-leaves-empty-level 1 \
  'if (level.head == kNilIndex) erase_level(order->side, order->price);' \
  ';'
check cancel-leaks-pool-slot 1 \
  'pool_.free(h);' \
  ';'
check fill-leaves-id-map-entry 1 \
  'id_map_.erase(maker_id);' \
  ';'
check rest-failure-leaks-level 1 \
  'if (level.head == kNilIndex) erase_level(side, price);' \
  ';'
check cancel-leaves-id-map-entry 3 \
  'id_map_.erase(order_id);' \
  ';'

# Zero allocation (test 25): a result that gives its trade storage back
# makes every command that trades allocate on its next one.
check result-reset-drops-capacity 1 \
  'trades.clear();' \
  'trades.clear(); trades.shrink_to_fit();'

# The order id lookup (ADR-005)
check book-limit-bad-side-accepted 1 \
  'if (!valid(side)) return;' \
  ';'
check book-market-bad-side-accepted 2 \
  'if (!valid(side)) return;' \
  ';'
check book-limit-bad-tif-accepted 1 \
  'if (!valid(tif)) return;' \
  ';'

use_target include/engine/id_map.hpp
check idmap-table-too-small 1 \
  'while (slots < 2 * static_cast<std::size_t>(max_entries)) slots *= 2;' \
  'while (slots < static_cast<std::size_t>(max_entries)) slots *= 2;'
check idmap-find-ignores-key 1 \
  'if (e.id == id) return &e.handle;' \
  'return &e.handle;'
check idmap-contains-inverted 1 \
  'return find(id) != nullptr;' \
  'return find(id) == nullptr;'
check idmap-insert-does-not-wrap 1 \
  'while (table_[i].handle.valid()) i = (i + 1) & mask_;' \
  'while (table_[i].handle.valid()) i = i + 1;'
check idmap-insert-over-capacity-accepted 1 \
  'if (!handle.valid() || size_ >= max_entries_) return false;' \
  'if (!handle.valid()) return false;'
check idmap-insert-refuses-one-early 1 \
  'size_ >= max_entries_) return false;' \
  'size_ + 1 >= max_entries_) return false;'
check idmap-insert-invalid-handle-accepted 1 \
  'if (!handle.valid() || size_ >= max_entries_) return false;' \
  'if (size_ >= max_entries_) return false;'
check idmap-insert-refusal-counted 1 \
  'if (!handle.valid() || size_ >= max_entries_) return false;' \
  'if (!handle.valid() || size_ >= max_entries_) { ++size_; return false; }'
check idmap-insert-not-counted 1 \
  '++size_;' \
  ';'
check idmap-erase-not-counted 1 \
  '--size_;' \
  ';'
check idmap-erase-keeps-entry 1 \
  'table_[hole].handle = Handle{};' \
  ';'
check idmap-no-backward-shift 1 \
  'if (((i - home(table_[i].id)) & mask_) >= ((i - hole) & mask_)) {' \
  'if (false) {'
check idmap-shift-skips-home-slot 1 \
  '>= ((i - hole) & mask_)) {' \
  '> ((i - hole) & mask_)) {'

# The price levels (ADR-003). A mutant that would make the matching loop spin
# forever, such as one that stops pop_best() from shrinking the array, is
# left out on purpose: it would only be "killed" by the test timeout.
use_target include/engine/price_levels.hpp
check levels-better-inverted 1 \
  'return B == Best::High ? a > b : a < b;' \
  'return B == Best::High ? a < b : a > b;'
check levels-best-is-first 1 \
  'return levels_[count_ - 1];' \
  'return levels_[0];'
check levels-rank-off-by-one 1 \
  'return levels_[count_ - 1 - rank];' \
  'return levels_[count_ - rank];'
check levels-search-reversed 1 \
  '[](const Level& l, Price p) { return better(p, l.price); });' \
  '[](const Level& l, Price p) { return better(l.price, p); });'
check levels-find-ignores-price 1 \
  'return at != end() && at->price == price ? at : nullptr;' \
  'return at != end() ? at : nullptr;'
check levels-duplicate-level 1 \
  'if (at != end() && at->price == price) return *at;' \
  'if (false) return *at;'
check levels-insert-does-not-shift 1 \
  'std::copy_backward(at, end(), end() + 1);' \
  ';'
check levels-insert-not-counted 1 \
  '++count_;' \
  ';'
check levels-new-level-not-empty 1 \
  '*at = Level{price, kNilIndex, kNilIndex};' \
  '*at = Level{price, 0, 0};'
check levels-erase-does-not-shift 1 \
  'std::copy(at + 1, end(), at);' \
  ';'
check levels-erase-not-counted 2 \
  '--count_;' \
  ';'

# The command journal (ADR-010)
use_target include/engine/journal.hpp
check journal-crc-no-final-xor 1 \
  'return c ^ 0xFFFFFFFFu;' \
  'return c;'
check journal-crc-no-initial-value 1 \
  'std::uint32_t c = 0xFFFFFFFFu;' \
  'std::uint32_t c = 0;'
check journal-crc-wrong-polynomial 1 \
  '0xEDB88320u ^' \
  '0xEDB88321u ^'
check journal-capacity-not-stored 1 \
  'detail::put(out + 12, pool_capacity, 4);' \
  ';'
check journal-limit-fields-swapped 1 \
  'detail::put(p, c.account, 4);' \
  'detail::put(p, c.qty, 4);'
check journal-price-read-narrow 1 \
  'c.price = static_cast<Price>(detail::get(p + 2, 8));' \
  'c.price = static_cast<Price>(detail::get(p + 2, 4));'
check journal-magic-unchecked 1 \
  'if (data_[i] != static_cast<std::uint8_t>(kMagic[i])) return Status::BadField;' \
  ';'
check journal-header-crc-unchecked 1 \
  'if (crc32(data_, 16) != detail::get(data_ + 16, 4)) return Status::BadChecksum;' \
  ';'
check journal-version-unchecked 1 \
  'if (detail::get(data_ + 8, 4) != kVersion) return Status::BadField;' \
  ';'
check journal-unknown-kind-accepted 1 \
  'if (body == 1) return Status::BadKind;' \
  ';'
check journal-truncated-reads-as-end 1 \
  'if (size_ - pos_ < body + 4) return Status::Truncated;' \
  'if (size_ - pos_ < body + 4) return Status::End;'
check journal-record-crc-unchecked 1 \
  'if (crc32(rec, body) != detail::get(rec + body, 4)) return Status::BadChecksum;' \
  ';'
check journal-limit-side-and-tif-unchecked 1 \
  'if (p[0] > 1 || p[1] > 2) return Status::BadField;' \
  ';'
check journal-limit-tif-unchecked 1 \
  'p[0] > 1 || p[1] > 2' \
  'p[0] > 1'
check journal-market-side-unchecked 1 \
  'if (p[0] > 1) return Status::BadField;' \
  ';'
check journal-reader-does-not-advance 1 \
  'pos_ += body + 4;' \
  ';'
check journal-writer-no-flush 1 \
  'std::fwrite(bytes, 1, n, file_) != n || std::fflush(file_) != 0' \
  'std::fwrite(bytes, 1, n, file_) != n'
check journal-writer-ignores-failure 1 \
  'std::fwrite(bytes, 1, n, file_) != n || std::fflush(file_) != 0' \
  'false'
check journal-writer-skips-header 1 \
  'return write(header, kHeaderSize);' \
  'return true;'

# Which commands the journal accepts (ADR-011). The precedence mutant keeps
# the Invalid check but lets a writer that cannot write answer first.
check journal-encode-writes-invalid-command 1 \
  'if (!valid(c)) return 0;' \
  ';'
check journal-append-invalid-unchecked 1 \
  'if (!valid(c)) return AppendStatus::Invalid;' \
  ';'
check journal-append-unusable-beats-invalid 1 \
  'if (!valid(c)) return AppendStatus::Invalid;' \
  'if (!valid(c) && file_ != nullptr && !failed_) return AppendStatus::Invalid;'
check journal-append-unusable-unchecked 1 \
  'if (file_ == nullptr || failed_) return AppendStatus::Unusable;' \
  ';'
check journal-append-io-error-reported-as-ok 1 \
  '? AppendStatus::Ok : AppendStatus::IoError;' \
  '? AppendStatus::Ok : AppendStatus::Ok;'

use_target include/engine/types.hpp
check types-sell-not-a-side 1 \
  'return s == Side::Buy || s == Side::Sell;' \
  'return s == Side::Buy;'
check types-third-side-valid 1 \
  'return s == Side::Buy || s == Side::Sell;' \
  'return s == Side::Buy || s == Side::Sell || s == static_cast<Side>(2);'

check types-fok-not-a-tif 1 \
  't == TimeInForce::IOC || t == TimeInForce::FOK' \
  't == TimeInForce::IOC'
check types-fourth-tif-valid 1 \
  't == TimeInForce::IOC || t == TimeInForce::FOK' \
  't == TimeInForce::IOC || t == TimeInForce::FOK || t == static_cast<TimeInForce>(3)'

use_target include/engine/command.hpp
check command-limit-kind-not-valid 1 \
  'case CommandKind::Limit:' \
  'case static_cast<CommandKind>(0):'
check command-fifth-kind-valid 1 \
  'case CommandKind::Modify:' \
  'case static_cast<CommandKind>(5):'
check command-valid-ignores-kind 1 \
  'if (!valid(c.kind)) return false;' \
  ';'
check command-valid-ignores-side 1 \
  'return valid(c.side);' \
  'return true;'
check command-cancel-side-checked 1 \
  'c.kind == CommandKind::Market) return valid(c.side);' \
  'c.kind != CommandKind::Limit) return valid(c.side);'
check command-valid-ignores-tif 1 \
  'if (c.kind == CommandKind::Limit && !valid(c.tif)) return false;' \
  ';'
check command-market-tif-checked 1 \
  'c.kind == CommandKind::Limit && !valid(c.tif)' \
  'c.kind != CommandKind::Cancel && !valid(c.tif)'
check command-market-side-unchecked 1 \
  'c.kind == CommandKind::Limit || c.kind == CommandKind::Market' \
  'c.kind == CommandKind::Limit'

# Replay and the event log (ADR-006). The golden files are what pin these:
# live and replayed runs share apply() and would agree on a wrong one.
use_target include/engine/replay.hpp
check replay-cancel-result-inverted 1 \
  'o.ok = book.cancel_order(c.id);' \
  'o.ok = !book.cancel_order(c.id);'
check replay-modify-quantity-off 1 \
  'book.modify_order(c.id, c.price, c.qty)' \
  'book.modify_order(c.id, c.price, c.qty + 1)'
check replay-market-ignores-account 1 \
  'book.add_market_order(scratch, c.id, c.side, c.qty, c.account);' \
  'book.add_market_order(scratch, c.id, c.side, c.qty, 0);'
check replay-limit-ignores-tif 1 \
  'c.account, c.tif);' \
  'c.account);'
check replay-resting-qty-dropped 1 \
  'o.resting_qty = scratch.resting_qty;' \
  ';'
check replay-stp-flag-dropped 1 \
  'o.stp_rejected = scratch.stp_rejected;' \
  ';'
check replay-pool-flag-dropped 1 \
  'o.pool_exhausted = scratch.pool_exhausted;' \
  ';'
check replay-trade-lines-dropped 1 \
  'for (const Trade& t : trades) {' \
  'for (const Trade& t : trades) { continue;'
check replay-ignores-header-capacity 1 \
  'book.emplace(reader.pool_capacity());' \
  'book.emplace(4096);'
check replay-capacity-limit-off-by-one 1 \
  'reader.pool_capacity() > limits.max_pool_capacity' \
  'reader.pool_capacity() >= limits.max_pool_capacity'
check replay-capacity-limit-ignored 1 \
  'if (reader.pool_capacity() > limits.max_pool_capacity) {' \
  'if (false) {'
check replay-capacity-limit-not-reported 1 \
  'out.limit = Limit::PoolCapacity;' \
  'out.limit = Limit::None;'
check replay-oom-not-caught 1 \
  'catch (const std::bad_alloc&) {' \
  'catch (const std::bad_array_new_length&) {'
check replay-oom-not-reported 1 \
  'out.limit = Limit::OutOfMemory;' \
  'out.limit = Limit::None;'
check replay-trade-reserve-dropped 1 \
  'scratch.trades.reserve(reader.pool_capacity());' \
  ';'
check replay-position-off-by-one 1 \
  'append_events(out.log, out.commands, c, o, scratch.trades);' \
  'append_events(out.log, out.commands + 1, c, o, scratch.trades);'
check replay-commands-not-counted 1 \
  '++out.commands;' \
  ';'
check replay-valid-bytes-not-set 1 \
  'out.valid_bytes = reader.valid_bytes();' \
  ';'
check replay-book-not-serialized 1 \
  'out.book = serialize_book(*book);' \
  ';'
check replay-book-sides-swapped 1 \
  'o.side == Side::Buy ? "bid" : "ask"' \
  'o.side == Side::Buy ? "ask" : "bid"'
check replay-bad-header-ignored 1 \
  'if (out.stop != journal::Status::Ok) return false;' \
  ';'
check replay-bad-side-printed-as-sell 1 \
  'if (side == Side::Sell) return "sell";' \
  'return "sell";'
check replay-bad-tif-printed-as-fok 1 \
  'if (tif == TimeInForce::FOK) return "fok";' \
  'return "fok";'
check replay-invalid-command-applied 1 \
  'if (!valid(c)) {' \
  'if (false) {'
check replay-invalid-command-keeps-trades 1 \
  'scratch.reset();' \
  ';'

echo
killed=$((total - ${#survivors[@]}))
echo "$killed/$total killed"
if [ "$broken" -ne 0 ]; then
  echo "some mutants could not be applied or built; a header has drifted from this list"
  exit 2
fi
if [ ${#survivors[@]} -ne 0 ]; then
  echo "survivors: ${survivors[*]}"
  exit 1
fi
