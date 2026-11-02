#include "matchbox/alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
// Relaxed: a monotonic statistic read only by the thread doing the measurement.
std::atomic<std::uint64_t> g_allocs{0};

[[maybe_unused]] void* counted_alloc(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  std::abort();
}
[[maybe_unused]] void* counted_aligned_alloc(std::size_t n, std::size_t align) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = nullptr;
  if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, n ? n : 1) == 0) return p;
  std::abort();
}
}  // namespace

namespace matchbox {
std::uint64_t alloc_count() noexcept { return g_allocs.load(std::memory_order_relaxed); }
bool alloc_hook_enabled() noexcept {
#if defined(MATCHBOX_NO_ALLOC_HOOK)
  return false;
#else
  return true;
#endif
}
}  // namespace matchbox

// ThreadSanitizer's runtime defines its own operator new/delete; the counting hook is compiled
// out there (the allocation test then skips itself).
#if !defined(MATCHBOX_NO_ALLOC_HOOK)

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t a) {
  return counted_aligned_alloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return counted_aligned_alloc(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif  // MATCHBOX_NO_ALLOC_HOOK
