// Property tests (RapidCheck): invariants from docs/RULES.md over random order flows.
#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <unordered_map>

#include "harness.hpp"
#include "matchbox/gen.hpp"

using namespace matchbox;
using namespace matchbox::test;

namespace {
struct Track {
  Qty base = 0;     // original qty (re-based on replace)
  Qty filled = 0;   // since base
  Qty leaves = 0;
  Price limit = 0;  // 0 = market
  Side side = Side::Buy;
  bool done = false;
};
}  // namespace

RC_GTEST_PROP(Property, InvariantsHoldOnRandomFlow, ()) {
  GenConfig cfg;
  cfg.seed = *rc::gen::arbitrary<std::uint64_t>();
  cfg.accounts = *rc::gen::inRange<std::uint32_t>(1, 8);
  cfg.spread_ticks = *rc::gen::inRange<Price>(1, 60);
  cfg.p_stop = *rc::gen::element(0.0, 0.05, 0.2);
  Gen g(cfg);
  Fast x;
  std::unordered_map<OrderId, Track> t;
  for (int i = 0; i < 1500; ++i) {
    const Input in = g.next();
    const auto ev = x.run(in);
    for (std::size_t k = 0; k < ev.size(); ++k) {
      const Event& e = ev[k];
      RC_ASSERT(e.seq == in.seq);
      switch (e.type) {
        case EventType::Accepted:
          t[e.order_id] = Track{e.qty, 0, e.qty, e.price, e.side, false};
          break;
        case EventType::Fill: {
          // Fills are emitted strictly as (taker, maker) pairs with equal qty/price, opposite
          // sides and cross-referenced ids.
          RC_ASSERT(e.qty > 0);
          RC_ASSERT(k + 1 < ev.size());
          const Event& m = ev[k + 1];
          RC_ASSERT(m.type == EventType::Fill);
          RC_ASSERT(m.order_id == e.other_id);
          RC_ASSERT(m.other_id == e.order_id);
          RC_ASSERT(m.qty == e.qty);
          RC_ASSERT(m.price == e.price);
          RC_ASSERT(m.side != e.side);
          auto& tt = t[e.order_id];
          // No trade-through: a limit taker never pays more (buy) / receives less (sell).
          if (tt.limit > 0) RC_ASSERT((e.side == Side::Buy ? e.price <= tt.limit : e.price >= tt.limit));
          tt.filled += e.qty;
          tt.leaves = e.leaves;
          auto& mt = t[m.order_id];
          RC_ASSERT(mt.limit == m.price);  // makers trade at their own resting price
          mt.filled += m.qty;
          mt.leaves = m.leaves;
          ++k;
          break;
        }
        case EventType::Rested:
          t[e.order_id].leaves = e.leaves;
          break;
        case EventType::Cancelled: {
          auto& tr = t[e.order_id];
          RC_ASSERT(e.qty == tr.leaves);  // cancels exactly what was left
          tr.leaves = 0;
          tr.done = true;
          break;
        }
        case EventType::Replaced: {
          auto& tr = t[e.order_id];
          tr.base = tr.filled + e.leaves;
          tr.leaves = e.leaves;
          tr.limit = e.price;
          break;
        }
        default: break;
      }
    }
    // Book never crossed once an input completes.
    auto b = x.levels(Side::Buy), a = x.levels(Side::Sell);
    if (!b.empty() && !a.empty()) RC_ASSERT(std::get<0>(b.front()) < std::get<0>(a.front()));
  }
  // Conservation and level aggregates.
  for (auto& [id, tr] : t) RC_ASSERT(tr.filled + tr.leaves <= tr.base + tr.filled);  // sanity
  for (auto& [id, tr] : t)
    if (!tr.done) RC_ASSERT(tr.base == tr.filled + tr.leaves);
  for (Side s : {Side::Buy, Side::Sell})
    for (auto& [px, leaves, visible, n] : x.levels(s)) {
      Qty l = 0, v = 0;
      std::size_t c = 0;
      for (auto& [id, ol, ov] : x.queue(s, px)) {
        RC_ASSERT(ov > 0);
        RC_ASSERT(ov <= ol);
        l += ol;
        v += ov;
        ++c;
      }
      RC_ASSERT(l == leaves);
      RC_ASSERT(v == visible);
      RC_ASSERT(c == n);
    }
}
