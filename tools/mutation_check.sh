#!/usr/bin/env bash
# Checks that the test suite notices when include/engine/book.hpp or
# include/engine/id_map.hpp is wrong.
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
  'if (available < qty)' \
  'if (available <= qty)'
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
  'order->seq_no = next_seq_++;' \
  'order->seq_no = next_seq_;'

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

# The order id lookup (ADR-005)
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
