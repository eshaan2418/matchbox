// Bounded lock-free single-producer / single-consumer ring.
//
// Used to hand BBO updates from the feed-handler thread to a strategy thread. Design:
//  - power-of-two capacity, monotonically increasing 64-bit head/tail (no ABA, no wrap logic);
//  - producer and consumer indices live on separate cache lines, padded to 128 bytes: Apple M-series
//    reports hw.cachelinesize = 128 and adjacent-line prefetch on x86 makes 128 the safe choice
//    there too. Each side also keeps a private cached copy of the other side's index so the
//    shared line is only read when the ring looks full/empty;
//  - acquire/release only: the producer's release store of tail publishes the slot write; the
//    consumer's acquire load of tail makes it visible. Symmetrically for head (slot reuse).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace matchbox {

inline constexpr std::size_t kCacheLine = 128;

template <class T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  explicit SpscRing(std::size_t capacity_pow2)
      : mask_(capacity_pow2 - 1), slots_(std::make_unique<T[]>(capacity_pow2)) {}

  // Producer only. Returns false if full (caller decides: drop, spin, or count a slow consumer).
  bool try_push(const T& v) noexcept {
    const std::uint64_t t = prod_.tail.load(std::memory_order_relaxed);  // only we write tail
    if (t - prod_.head_cache > mask_) {
      // Looks full: refresh our view of the consumer. Acquire pairs with the consumer's release
      // of head, so the slot we are about to overwrite has been fully read.
      prod_.head_cache = cons_.head.load(std::memory_order_acquire);
      if (t - prod_.head_cache > mask_) return false;
    }
    slots_[t & mask_] = v;
    prod_.tail.store(t + 1, std::memory_order_release);  // publish the slot write
    return true;
  }

  // Consumer only.
  bool try_pop(T& out) noexcept {
    const std::uint64_t h = cons_.head.load(std::memory_order_relaxed);  // only we write head
    if (h == cons_.tail_cache) {
      cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);  // pairs with push release
      if (h == cons_.tail_cache) return false;
    }
    out = slots_[h & mask_];
    cons_.head.store(h + 1, std::memory_order_release);  // slot may now be reused
    return true;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1; }

 private:
  struct alignas(kCacheLine) Producer {
    std::atomic<std::uint64_t> tail{0};
    std::uint64_t head_cache = 0;
  };
  struct alignas(kCacheLine) Consumer {
    std::atomic<std::uint64_t> head{0};
    std::uint64_t tail_cache = 0;
  };
  Producer prod_;
  Consumer cons_;
  const std::uint64_t mask_;
  std::unique_ptr<T[]> slots_;
};

}  // namespace matchbox
