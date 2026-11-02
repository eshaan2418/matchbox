// Reproducible random order flow covering every order type, TIF, STP mode, iceberg, stop,
// cancel and replace (including ones aimed at dead/unknown/foreign orders). Prices random-walk
// around a mid so the book is two-sided with occasional sweeps. Used by tests, the fuzzers'
// seed corpus, benchmarks and the load generator.
#pragma once

#include <cstdint>
#include <vector>

#include "matchbox/types.hpp"

namespace matchbox {

struct GenConfig {
  std::uint64_t seed = 1;
  std::uint32_t accounts = 4;     // small => frequent self-trade situations
  Price mid = 10'000;             // ticks
  Price spread_ticks = 20;        // limit prices drawn within +/- this of mid
  double p_market = 0.06, p_ioc = 0.08, p_fok = 0.05, p_post = 0.06, p_iceberg = 0.06,
         p_stop = 0.04, p_cancel = 0.15, p_replace = 0.10, p_stp = 0.25, p_invalid = 0.01;
  bool realistic = false;  // benchmark mode: no invalid inputs, fewer exotic types
};

class Gen {
 public:
  explicit Gen(const GenConfig& c);
  Input next();  // seq increases by 1 per call
  [[nodiscard]] std::uint64_t seq() const { return seq_; }

 private:
  std::uint64_t rnd();
  double uni();
  std::uint64_t below(std::uint64_t n);
  GenConfig c_;
  std::uint64_t s_[4];
  std::uint64_t seq_ = 0;
  Price mid_;
  std::vector<std::pair<OrderId, Account>> ids_;  // recent submitted orders (targets)
};

}  // namespace matchbox
