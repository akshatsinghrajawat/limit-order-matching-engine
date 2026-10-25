#!/usr/bin/env bash
# Produces the three results files a published baseline needs, in the order
# docs/benchmark-methodology.md requires: the noise floor (test 26), the
# measurement overhead (test 27), then W1 itself.
#
# Usage: tools/run_baseline.sh <cpu> <label>
#
# <cpu> is the core to pin to. Pick one nothing else is using, not core 0,
# and ideally one whose hyperthread sibling is idle (lscpu -e shows the
# pairs). <label> names the setup, e.g. ubuntu-i5-1235u, and prefixes the
# files in bench/results/.
#
# Linux only. The governor has to be `performance` and turbo has to be off
# (echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo). Power
# managers such as auto-cpufreq and power-profiles-daemon change both while
# a benchmark runs, so stop them first. The governor is checked before
# starting; both settings are checked again in what the results files
# recorded, which is read after the measurements. Set ALLOW_UNTUNED_MACHINE=1
# to skip the checks, for trying the script out on a machine that can't set
# them. The files still record what the settings were.
#
# Nothing lands in bench/results/ unless every step succeeds, and an
# existing file is never overwritten.
#
# Exit status: 0 done, 1 the machine failed a check, 2 bad usage or setup.

set -eu

if [ $# -ne 2 ]; then
  echo "usage: tools/run_baseline.sh <cpu> <label>" >&2
  exit 2
fi
cpu=$1
label=$2

case $cpu in
  '' | *[!0-9]*) echo "cpu must be a number, got '$cpu'" >&2; exit 2 ;;
esac
case $label in
  '' | *[!A-Za-z0-9._-]*) echo "label may only contain letters, digits, . _ -" >&2; exit 2 ;;
esac

# True if the results file says the governor was performance and turbo was
# disabled when it was written.
recorded_tuned() {
  grep -qx '# governor: performance' "$1" && grep -qx '# turbo: disabled' "$1"
}

root=$(cd "$(dirname "$0")/.." && pwd)
results=$root/bench/results
build=$root/cmake-build-bench

if [ "$(uname -s)" != Linux ]; then
  echo "published numbers come from Linux runs (see the methodology doc)" >&2
  exit 2
fi

for kind in noise overhead w1; do
  if [ -e "$results/$label-$kind.csv" ]; then
    echo "$results/$label-$kind.csv already exists; pick another label" >&2
    exit 2
  fi
done

governor_file=/sys/devices/system/cpu/cpu$cpu/cpufreq/scaling_governor
governor=$(cat "$governor_file" 2>/dev/null || echo unavailable)
if [ "$governor" != performance ] && [ "${ALLOW_UNTUNED_MACHINE:-0}" != 1 ]; then
  echo "cpu $cpu governor is '$governor', the methodology needs 'performance'." >&2
  echo "  sudo cpupower frequency-set -g performance" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF \
  -DCMAKE_CXX_FLAGS="-O3 -march=native -fno-plt" > "$work/configure.log" 2>&1 || {
  cat "$work/configure.log"
  exit 2
}
cmake --build "$build" --parallel --target bench_noise bench_overhead bench_w1 > "$work/build.log" 2>&1 || {
  cat "$work/build.log"
  exit 2
}

echo "noise floor (test 26)"
"$build/bench_noise" "$cpu" > "$work/noise.csv" || {
  echo "the noise floor check failed on cpu $cpu; no numbers from this setup can be trusted" >&2
  exit 1
}
echo "measurement overhead (test 27)"
"$build/bench_overhead" 1 1000000 "$cpu" > "$work/overhead.csv"
echo "W1"
"$build/bench_w1" "$label" 1 1000000 "$cpu" > "$work/w1.csv"

if [ "${ALLOW_UNTUNED_MACHINE:-0}" != 1 ]; then
  for kind in noise overhead w1; do
    if ! recorded_tuned "$work/$kind.csv"; then
      echo "the $kind results don't record governor performance and turbo disabled:" >&2
      grep -h '^# \(governor\|turbo\):' "$work/$kind.csv" >&2
      echo "a power manager probably changed them during the run; stop it and retry" >&2
      exit 1
    fi
  done
fi

for kind in noise overhead w1; do
  cp "$work/$kind.csv" "$results/$label-$kind.csv"
done
grep -h '^# p99_spread_ns\|^# one_percent_rule\|^# throughput_ops_per_sec' \
  "$results/$label-noise.csv" "$results/$label-overhead.csv" "$results/$label-w1.csv"
echo "wrote $results/$label-{noise,overhead,w1}.csv"
