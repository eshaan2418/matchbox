// Open-addressing hash map: order id (u64) -> order pool index (u32).
//
// - Linear probing over 16-byte slots (8 slots per 128-byte Apple-silicon cache line), so a probe
//   sequence almost always stays within one or two lines.
// - Fibonacci (multiplicative) hashing: order ids are assigned in increasing order by the sequencer, and the
//   golden-ratio multiply scatters consecutive keys across the table.
// - Backward-shift deletion (no tombstones): the table never degrades over a trading day with
//   hundreds of millions of insert/erase pairs.
// - Capacity is a power of two chosen up front; growth (rehash) exists only as a slow path and
//   is counted, so the hot path is allocation-free once the table is sized for the peak.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace matchbox {

class OrderMap {
 public:
  static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};
  static constexpr std::uint32_t kNotFound = ~std::uint32_t{0};

  explicit OrderMap(std::size_t min_capacity = 1u << 16) { reset(min_capacity); }

  void reset(std::size_t min_capacity) {
    std::size_t cap = 16;
    while (cap < min_capacity) cap <<= 1;
    slots_.assign(cap, Slot{kEmpty, 0});
    mask_ = cap - 1;
    shift_ = 64 - static_cast<unsigned>(__builtin_ctzll(cap));
    size_ = 0;
    grow_at_ = cap / 10 * 7;  // max load factor 0.7
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
  [[nodiscard]] std::size_t rehashes() const noexcept { return rehashes_; }

  // Returns the stored value or kNotFound.
  [[nodiscard]] std::uint32_t find(std::uint64_t key) const noexcept {
    std::size_t i = home(key);
    for (;;) {
      const Slot& s = slots_[i];
      if (s.key == key) return s.value;
      if (s.key == kEmpty) return kNotFound;
      i = (i + 1) & mask_;
    }
  }

  // Inserts key -> value. Returns false (and leaves the map unchanged) if key already exists.
  bool insert(std::uint64_t key, std::uint32_t value) {
    if (size_ >= grow_at_) [[unlikely]] grow();
    std::size_t i = home(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == kEmpty) {
        s.key = key;
        s.value = value;
        ++size_;
        return true;
      }
      if (s.key == key) return false;
      i = (i + 1) & mask_;
    }
  }

  // Removes key and returns its value, or kNotFound.
  std::uint32_t erase(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == key) break;
      if (s.key == kEmpty) return kNotFound;
      i = (i + 1) & mask_;
    }
    const std::uint32_t value = slots_[i].value;
    // Backward-shift: pull later members of the probe run into the hole if their home slot
    // does not lie cyclically in (hole, j].
    std::size_t hole = i;
    std::size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      const Slot& sj = slots_[j];
      if (sj.key == kEmpty) break;
      const std::size_t h = home(sj.key);
      const bool movable = ((j - h) & mask_) >= ((j - hole) & mask_);
      if (movable) {
        slots_[hole] = sj;
        hole = j;
      }
    }
    slots_[hole].key = kEmpty;
    --size_;
    return value;
  }

  // Bring the home slot of `key` towards L1 ahead of use.
  void prefetch(std::uint64_t key) const noexcept {
    __builtin_prefetch(&slots_[home(key)], 0, 3);
  }

 private:
  struct Slot {
    std::uint64_t key;
    std::uint32_t value;
  };
  static_assert(sizeof(Slot) == 16, "16-byte slots");

  [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_) & mask_;
  }

  void grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    reset(old.size() * 2);
    for (const Slot& s : old)
      if (s.key != kEmpty) insert(s.key, s.value);
    ++rehashes_;
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  unsigned shift_ = 0;
  std::size_t size_ = 0;
  std::size_t grow_at_ = 0;
  std::size_t rehashes_ = 0;
};

}  // namespace matchbox
