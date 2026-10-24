// Checks the timing primitives and the environment block. The parsers are
// tested on fixed text so the result doesn't depend on the machine; the
// clock and the block itself are checked against what the machine reports.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "env.hpp"
#include "timing.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace bench;

static void tsc_reads_never_go_backwards() {
  std::uint64_t prev = tsc_now();
  const std::uint64_t first = prev;
  for (int i = 0; i < 100000; ++i) {
    std::uint64_t t = tsc_now();
    CHECK(t >= prev);
    prev = t;
  }
  CHECK(prev > first);
}

static void clock_agrees_with_steady_clock_over_a_sleep() {
  const TscClock clock;
  // GHz-range counters: anything outside this is a calibration bug, not a
  // slow machine.
  CHECK(clock.ticks_per_ns() > 0.1 && clock.ticks_per_ns() < 10.0);

  TscSample a = sample_pair();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  TscSample b = sample_pair();

  double steady_ns = static_cast<double>(b.ns - a.ns);
  double tsc_ns = clock.to_ns(b.ticks - a.ticks);
  std::printf("50 ms window: steady=%.0f ns tsc=%.0f ns\n", steady_ns, tsc_ns);
  CHECK(steady_ns >= 50e6);
  CHECK(tsc_ns > steady_ns * 0.99 && tsc_ns < steady_ns * 1.01);
}

static void parsers_on_fixed_text() {
  const std::string cpuinfo =
      "processor\t: 0\n"
      "vendor_id\t: GenuineIntel\n"
      "model name\t: Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz\n"
      "model name\t: second core, ignored\n";
  CHECK(cpu_model_from_cpuinfo(cpuinfo) == "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz");
  CHECK(cpu_model_from_cpuinfo("processor\t: 0\n") == kUnavailable);

  const std::string meminfo =
      "MemTotal:       16314672 kB\n"
      "MemFree:         1203948 kB\n";
  CHECK(ram_from_meminfo(meminfo) == "15932 MiB");
  CHECK(ram_from_meminfo("MemFree:  100 kB\n") == kUnavailable);
  CHECK(ram_from_meminfo("MemTotal: 0 kB\n") == kUnavailable);

  CHECK(trim("  \t value \r\n") == "value");
  CHECK(trim("   ") == "");
}

static void fnv1a_matches_published_vectors() {
  CHECK(fnv1a("", 0) == 0xcbf29ce484222325ull);
  CHECK(fnv1a("a", 1) == 0xaf63dc4c8601ec8cull);
  CHECK(fnv1a("foobar", 6) == 0x85944171f73967e8ull);
  // Feeding the state back in must equal hashing the whole string at once.
  CHECK(fnv1a("bar", 3, fnv1a("foo", 3)) == fnv1a("foobar", 6));
  CHECK(hex64(0xabcull) == "0000000000000abc");
}

#ifdef __linux__
static void pinning_is_read_back_from_the_kernel() {
  int cpu = current_cpu();
  CHECK(cpu >= 0);
  CHECK(pin_to_cpu(cpu));
  CHECK(pinned_cpu() == cpu);
  CHECK(!pin_to_cpu(-1));
  CHECK(!pin_to_cpu(1 << 20));
  CHECK(pinned_cpu() == cpu);  // failed attempts leave the mask alone
}
#endif

static void block_has_every_field_once_and_none_empty() {
  const TscClock clock;
  const std::string block = environment_block(clock);
  std::fputs(block.c_str(), stdout);

  const char* keys[] = {"cpu_model", "pinned_cpu", "governor", "turbo", "ram",
                        "kernel",    "compiler",   "flags",    "tsc",   "binary_hash"};
  std::size_t lines = 0;
  for (char ch : block) lines += ch == '\n';
  CHECK(lines == sizeof keys / sizeof keys[0]);
  CHECK(block.back() == '\n');

  for (const char* key : keys) {
    std::string prefix = std::string("# ") + key + ": ";
    std::size_t at = block.find(prefix);
    CHECK(at != std::string::npos);
    CHECK(block.find(prefix, at + 1) == std::string::npos);
    std::size_t end = block.find('\n', at);
    CHECK(end > at + prefix.size());  // a value is present
  }

  // CMake passes both; a block that says "unavailable" here means the
  // definitions never reached this target.
  CHECK(block.find("# compiler: unavailable") == std::string::npos);
  CHECK(block.find("# flags: unavailable") == std::string::npos);
#ifdef __linux__
  // Governor and turbo are routinely hidden inside containers and VMs, so
  // they aren't required. These four come from /proc and uname.
  CHECK(block.find("# cpu_model: unavailable") == std::string::npos);
  CHECK(block.find("# ram: unavailable") == std::string::npos);
  CHECK(block.find("# kernel: unavailable") == std::string::npos);
  CHECK(block.find("# binary_hash: unavailable") == std::string::npos);
#endif
}

int main() {
  tsc_reads_never_go_backwards();
  clock_agrees_with_steady_clock_over_a_sleep();
  parsers_on_fixed_text();
  fnv1a_matches_published_vectors();
#ifdef __linux__
  pinning_is_read_back_from_the_kernel();
#endif
  block_has_every_field_once_and_none_empty();
  std::puts("timing ok");
}
