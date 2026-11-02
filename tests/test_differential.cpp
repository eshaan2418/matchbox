// Differential tests: optimised Engine vs ReferenceEngine, event-for-event and book-for-book,
// on long random flows (all order types, cancels/replaces incl. invalid ones).
#include <gtest/gtest.h>

#include "harness.hpp"
#include "matchbox/gen.hpp"

using namespace matchbox;
using namespace matchbox::test;

namespace {
void run(const GenConfig& cfg, int n) {
  Gen g(cfg);
  Fast a;
  Ref b;
  for (int i = 0; i < n; ++i) {
    const Input in = g.next();
    const auto ea = a.run(in);
    const auto eb = b.run(in);
    ASSERT_EQ(ea.size(), eb.size()) << "input " << i << " seq " << in.seq;
    for (std::size_t k = 0; k < ea.size(); ++k)
      ASSERT_TRUE(ea[k] == eb[k]) << "input " << i << " event " << k << " type " << int(ea[k].type) << " vs " << int(eb[k].type);
    if (i % 97 == 0) {
      ASSERT_EQ(a.levels(Side::Buy), b.levels(Side::Buy)) << i;
      ASSERT_EQ(a.levels(Side::Sell), b.levels(Side::Sell)) << i;
      ASSERT_EQ(a.resting(), b.resting());
      ASSERT_EQ(a.stops(), b.stops());
    }
  }
  ASSERT_EQ(a.levels(Side::Buy), b.levels(Side::Buy));
  ASSERT_EQ(a.levels(Side::Sell), b.levels(Side::Sell));
  for (auto& [px, l, v, n2] : a.levels(Side::Buy)) ASSERT_EQ(a.queue(Side::Buy, px), b.queue(Side::Buy, px));
  for (auto& [px, l, v, n2] : a.levels(Side::Sell)) ASSERT_EQ(a.queue(Side::Sell, px), b.queue(Side::Sell, px));
}
}  // namespace

TEST(Differential, DefaultMix) { run({.seed = 1}, 300'000); }
TEST(Differential, ManyAccountsFewSelfTrades) { run({.seed = 2, .accounts = 50}, 200'000); }
TEST(Differential, OneAccountAllSelfTrades) { run({.seed = 3, .accounts = 1}, 100'000); }
TEST(Differential, TightBookManySweeps) { run({.seed = 4, .spread_ticks = 3}, 200'000); }
TEST(Differential, WideBook) { run({.seed = 5, .spread_ticks = 400}, 150'000); }
TEST(Differential, StopHeavy) {
  GenConfig c{.seed = 6};
  c.p_stop = 0.25;
  run(c, 150'000);
}
TEST(Differential, RealisticMix) {
  GenConfig c{.seed = 7};
  c.realistic = true;
  run(c, 300'000);
}
