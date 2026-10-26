// Replaces the global operator new and delete with versions that count.
//
// Built into the alloc_counter library, which only the allocation
// measurements link. Each object file of a static library is pulled in
// when something it defines is referenced, and the executables that link it
// call alloc_snapshot() from this file, so the replacements come with it.
//
// Relaxed atomics are enough: the counters are read after the measured
// code has finished on the same thread.
#include "alloc_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_bytes{0};

void* counted_malloc(std::size_t size) {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(size, std::memory_order_relaxed);
  return std::malloc(size == 0 ? 1 : size);
}

void* counted_aligned(std::size_t size, std::size_t align) {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(size, std::memory_order_relaxed);
  if (size == 0) size = 1;
#ifdef _WIN32
  return _aligned_malloc(size, align);
#else
  // aligned_alloc wants the size to be a multiple of the alignment.
  return std::aligned_alloc(align, (size + align - 1) / align * align);
#endif
}

void release(void* p) { std::free(p); }

void release_aligned(void* p) {
#ifdef _WIN32
  _aligned_free(p);
#else
  std::free(p);
#endif
}

void* or_throw(void* p) {
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

}  // namespace

namespace bench {

AllocSnapshot alloc_snapshot() {
  return {g_calls.load(std::memory_order_relaxed), g_bytes.load(std::memory_order_relaxed)};
}

}  // namespace bench

void* operator new(std::size_t n) { return or_throw(counted_malloc(n)); }
void* operator new[](std::size_t n) { return or_throw(counted_malloc(n)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_malloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_malloc(n); }

void* operator new(std::size_t n, std::align_val_t a) {
  return or_throw(counted_aligned(n, static_cast<std::size_t>(a)));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return or_throw(counted_aligned(n, static_cast<std::size_t>(a)));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return counted_aligned(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

void operator delete(void* p, std::align_val_t) noexcept { release_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { release_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { release_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { release_aligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { release_aligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { release_aligned(p); }
