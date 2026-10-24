#pragma once

// The environment block that heads every results file, as specified in
// docs/benchmark-methodology.md. A field that can't be read says
// "unavailable" rather than being left out, so a results file shows what was
// and wasn't known about the machine. Only Linux fills in everything; the
// methodology doc says published numbers come from Linux runs.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>

#ifdef __linux__
#include <sched.h>
#include <sys/utsname.h>
#endif

#include "timing.hpp"

// Set by CMakeLists.txt for bench_support.
#ifndef BENCH_COMPILER
#define BENCH_COMPILER "unavailable"
#endif
#ifndef BENCH_FLAGS
#define BENCH_FLAGS "unavailable"
#endif

namespace bench {

inline constexpr const char* kUnavailable = "unavailable";

inline std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  // /proc files report size 0, so read until EOF instead of sizing first.
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  std::size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// The value after the colon on the first line starting with `key`.
inline std::string field_after_colon(const std::string& text, const std::string& key) {
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.compare(0, key.size(), key) != 0) continue;
    std::size_t colon = line.find(':');
    if (colon != std::string::npos) return trim(line.substr(colon + 1));
  }
  return "";
}

inline std::string cpu_model_from_cpuinfo(const std::string& cpuinfo) {
  std::string v = field_after_colon(cpuinfo, "model name");
  return v.empty() ? kUnavailable : v;
}

// MemTotal is in kB, which /proc/meminfo means as KiB.
inline std::string ram_from_meminfo(const std::string& meminfo) {
  std::string v = field_after_colon(meminfo, "MemTotal");
  if (v.empty()) return kUnavailable;
  std::uint64_t kib = std::strtoull(v.c_str(), nullptr, 10);
  if (kib == 0) return kUnavailable;
  return std::to_string(kib / 1024) + " MiB";
}

inline std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t h = 0xcbf29ce484222325ull) {
  const auto* p = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

inline std::string hex64(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// Hash of the running executable, so a results file can be tied to the
// exact binary that produced it.
inline std::string binary_hash() {
  auto bytes = read_file("/proc/self/exe");
  if (!bytes) return kUnavailable;
  return hex64(fnv1a(bytes->data(), bytes->size()));
}

inline int current_cpu() {
#ifdef __linux__
  return sched_getcpu();
#else
  return -1;
#endif
}

inline bool pin_to_cpu(int cpu) {
#ifdef __linux__
  if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof set, &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

// The core the thread is confined to, read back from the kernel instead of
// trusting what the caller asked for. -1 if the mask allows more than one.
inline int pinned_cpu() {
#ifdef __linux__
  cpu_set_t set;
  if (sched_getaffinity(0, sizeof set, &set) != 0 || CPU_COUNT(&set) != 1) return -1;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) return cpu;
  }
#endif
  return -1;
}

inline std::string governor() {
  int cpu = current_cpu();
  if (cpu < 0) return kUnavailable;
  auto v = read_file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor");
  return v && !trim(*v).empty() ? trim(*v) : kUnavailable;
}

// intel_pstate exposes no_turbo (1 = turbo off), other drivers expose boost
// (1 = turbo on).
inline std::string turbo_state() {
  if (auto v = read_file("/sys/devices/system/cpu/intel_pstate/no_turbo")) {
    return trim(*v) == "1" ? "disabled" : "enabled";
  }
  if (auto v = read_file("/sys/devices/system/cpu/cpufreq/boost")) {
    return trim(*v) == "1" ? "enabled" : "disabled";
  }
  return kUnavailable;
}

inline std::string kernel_version() {
#ifdef __linux__
  utsname u;
  if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
#endif
  return kUnavailable;
}

inline std::string environment_block(const TscClock& clock) {
  auto from_file = [](const char* path, std::string (*parse)(const std::string&)) {
    auto text = read_file(path);
    return text ? parse(*text) : std::string(kUnavailable);
  };

  char tsc[64];
  std::snprintf(tsc, sizeof tsc, "%s, %.4f GHz", tsc_is_invariant() ? "invariant" : "not invariant",
                clock.ticks_per_ns());
  int pinned = pinned_cpu();

  std::ostringstream out;
  auto line = [&out](const char* key, const std::string& value) { out << "# " << key << ": " << value << '\n'; };
  line("cpu_model", from_file("/proc/cpuinfo", cpu_model_from_cpuinfo));
  line("pinned_cpu", pinned >= 0 ? std::to_string(pinned) : "not pinned");
  line("governor", governor());
  line("turbo", turbo_state());
  line("ram", from_file("/proc/meminfo", ram_from_meminfo));
  line("kernel", kernel_version());
  line("compiler", BENCH_COMPILER);
  line("flags", trim(BENCH_FLAGS));
  line("tsc", tsc);
  line("binary_hash", binary_hash());
  return out.str();
}

}  // namespace bench
