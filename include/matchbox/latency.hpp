// Low-overhead timestamping and an HdrHistogram-style log-linear latency histogram.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace matchbox {

// Monotonic tick counter.
//  - arm64: reads the architected generic timer (CNTVCT_EL0) directly, preceded by ISB so the
//    read is not hoisted above the work being timed. On Apple silicon this counter runs at
//    24 MHz (41.67 ns per tick) — the same source as mach_absolute_time(), without the call.
//  - elsewhere: std::chrono::steady_clock (ns; TSC-backed via vDSO on Linux x86-64).
struct Clock {
#if defined(__aarch64__)
  static inline std::uint64_t now() noexcept {
    std::uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
  }
  static inline double ns_per_tick() noexcept {
    std::uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return 1e9 / static_cast<double>(f);
  }
#else
  static inline std::uint64_t now() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }
  static inline double ns_per_tick() noexcept {
    return 1e9 * std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den;
  }
#endif
};

// Log-linear histogram: exact below 2048, then 1024 linear sub-buckets per power of two
// (relative error < 0.1%), the same precision scheme as HdrHistogram with 3 significant digits.
class Histogram {
 public:
  static constexpr unsigned kSubBits = 11;
  static constexpr std::uint64_t kSub = 1ull << kSubBits;  // 2048
  static constexpr std::uint64_t kHalf = kSub / 2;         // 1024

  Histogram() : counts_(kSub + 53 * kHalf, 0) {}

  void record(std::uint64_t v) noexcept {
    ++counts_[index(v)];
    ++total_;
    max_ = std::max(max_, v);
    min_ = std::min(min_, v);
    sum_ += v;
  }
  void merge(const Histogram& o) {
    for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
    total_ += o.total_;
    max_ = std::max(max_, o.max_);
    min_ = std::min(min_, o.min_);
    sum_ += o.sum_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
  [[nodiscard]] double mean() const noexcept {
    return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
  }

  // Value at quantile q in [0,1] (lower bound of the containing bucket; exact below 2048).
  [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
    if (total_ == 0) return 0;
    const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(total_ - 1)) + 1;
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      acc += counts_[i];
      if (acc >= rank) return std::min(value_at(i), max_);
    }
    return max_;
  }

  // CSV of non-empty buckets: value,count (value in the same unit as recorded).
  [[nodiscard]] std::string to_csv() const {
    std::string s = "value,count\n";
    char buf[64];
    for (std::size_t i = 0; i < counts_.size(); ++i)
      if (counts_[i]) {
        std::snprintf(buf, sizeof buf, "%llu,%llu\n",
                      static_cast<unsigned long long>(value_at(i)),
                      static_cast<unsigned long long>(counts_[i]));
        s += buf;
      }
    return s;
  }

  static std::size_t index(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<std::size_t>(v);
    const unsigned msb = 63u - static_cast<unsigned>(__builtin_clzll(v));
    const unsigned shift = msb - (kSubBits - 1);
    return static_cast<std::size_t>(kSub + (shift - 1) * kHalf + ((v >> shift) - kHalf));
  }
  static std::uint64_t value_at(std::size_t i) noexcept {
    if (i < kSub) return i;
    const std::size_t k = i - kSub;
    const std::uint64_t shift = k / kHalf + 1;
    return (kHalf + k % kHalf) << shift;
  }

 private:
  std::vector<std::uint64_t> counts_;
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
  std::uint64_t min_ = ~0ull;
  std::uint64_t sum_ = 0;
};

}  // namespace matchbox
