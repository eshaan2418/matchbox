// Rule-by-rule tests (docs/RULES.md), run against both engines.
#include <gtest/gtest.h>

#include "harness.hpp"

using namespace matchbox;
using namespace matchbox::test;

template <class E>
class Rules : public ::testing::Test {
 protected:
  E x;
  std::uint64_t seq = 0;
  std::vector<Event> limit(Account a, Side s, Price p, Qty q, Tif t = Tif::GTC) { return x.run(make_limit(++seq, a, s, p, q, t)); }
  std::vector<Event> market(Account a, Side s, Qty q) { return x.run(make_market(++seq, a, s, q)); }
  std::vector<Event> input(Input in) {
    in.seq = ++seq;
    return x.run(in);
  }
};
using Engines = ::testing::Types<Fast, Ref>;
TYPED_TEST_SUITE(Rules, Engines);

TYPED_TEST(Rules, PriceTimePriorityAndMakerPrice) {
  this->limit(1, Side::Sell, 101, 5);  // id 1
  this->limit(2, Side::Sell, 100, 5);  // id 2 (better price)
  this->limit(3, Side::Sell, 100, 5);  // id 3 (same price, later)
  auto ev = this->limit(4, Side::Buy, 102, 12);  // sweeps 2, 3, then 2 of 1
  std::vector<std::tuple<OrderId, Price, Qty>> takers;
  for (auto& e : ev)
    if (e.type == EventType::Fill && e.order_id == 4) takers.emplace_back(e.other_id, e.price, e.qty);
  ASSERT_EQ(takers, (std::vector<std::tuple<OrderId, Price, Qty>>{{2, 100, 5}, {3, 100, 5}, {1, 101, 2}}));
  EXPECT_EQ(this->x.queue(Side::Sell, 101), (std::vector<std::tuple<OrderId, Qty, Qty>>{{1, 3, 3}}));
  EXPECT_EQ(this->x.resting(), 1u);
}

TYPED_TEST(Rules, IocAndMarketNeverRest) {
  this->limit(1, Side::Sell, 100, 5);
  auto ev = this->limit(2, Side::Buy, 100, 8, Tif::IOC);
  EXPECT_EQ(count(ev, EventType::Fill), 2);
  ASSERT_EQ(ev.back().type, EventType::Cancelled);
  EXPECT_EQ(ev.back().reason, Reason::IOC);
  EXPECT_EQ(ev.back().qty, 3);
  ev = this->market(3, Side::Sell, 4);  // empty bid side
  EXPECT_EQ(ev.back().reason, Reason::IOC);
  EXPECT_EQ(this->x.resting(), 0u);
}

TYPED_TEST(Rules, FokAllOrNothing) {
  this->limit(1, Side::Sell, 100, 5);
  this->limit(1, Side::Sell, 101, 5);
  auto ev = this->limit(2, Side::Buy, 101, 11, Tif::FOK);  // only 10 available
  EXPECT_EQ(count(ev, EventType::Fill), 0);
  EXPECT_EQ(ev.back().reason, Reason::FOK);
  EXPECT_EQ(this->x.resting(), 2u);
  ev = this->limit(2, Side::Buy, 101, 10, Tif::FOK);
  EXPECT_EQ(count(ev, EventType::Fill), 4);
  EXPECT_EQ(this->x.resting(), 0u);
}

TYPED_TEST(Rules, FokRespectsLimitAndSelfTrade) {
  this->limit(1, Side::Sell, 100, 5);
  this->limit(2, Side::Sell, 101, 5);
  auto ev = this->limit(3, Side::Buy, 100, 6, Tif::FOK);  // second level above limit
  EXPECT_EQ(ev.back().reason, Reason::FOK);
  Input in = make_limit(0, 2, Side::Buy, 101, 6, Tif::FOK);
  in.stp = Stp::CancelOldest;  // would meet own order at 101 before completion
  ev = this->input(in);
  EXPECT_EQ(ev.back().reason, Reason::FOK);
  EXPECT_EQ(count(ev, EventType::Fill), 0);
  EXPECT_EQ(this->x.resting(), 2u);
}

TYPED_TEST(Rules, PostOnlyRejectsWhenMarketable) {
  this->limit(1, Side::Sell, 100, 5);
  Input in = make_limit(0, 2, Side::Buy, 100, 5);
  in.post_only = 1;
  auto ev = this->input(in);
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].type, EventType::Rejected);
  EXPECT_EQ(ev[0].reason, Reason::PostOnlyWouldCross);
  in.price = 99;
  ev = this->input(in);
  EXPECT_EQ(ev.back().type, EventType::Rested);
}

TYPED_TEST(Rules, IcebergRefillLosesPriority) {
  Input ice = make_limit(0, 1, Side::Sell, 100, 10);
  ice.display_qty = 3;
  auto ev = this->input(ice);  // id 1, visible 3
  EXPECT_EQ(ev.back().qty, 3);
  this->limit(2, Side::Sell, 100, 2);  // id 2 behind the iceberg
  ev = this->limit(3, Side::Buy, 100, 3);  // consumes iceberg's visible slice
  EXPECT_EQ(this->x.queue(Side::Sell, 100), (std::vector<std::tuple<OrderId, Qty, Qty>>{{2, 2, 2}, {1, 7, 3}}));
  // A large taker sweeps the whole level including the reserve.
  ev = this->limit(4, Side::Buy, 100, 9);
  Qty filled = 0;
  for (auto& e : ev)
    if (e.type == EventType::Fill && e.order_id == 4) filled += e.qty;
  EXPECT_EQ(filled, 9);
  EXPECT_EQ(this->x.resting(), 0u);
}

TYPED_TEST(Rules, SelfTradePrevention) {
  this->limit(7, Side::Sell, 100, 5);  // id 1
  this->limit(8, Side::Sell, 100, 5);  // id 2
  Input t = make_limit(0, 7, Side::Buy, 100, 4);
  t.stp = Stp::CancelNewest;
  auto ev = this->input(t);  // meets own order first => taker cancelled, no fills
  EXPECT_EQ(count(ev, EventType::Fill), 0);
  EXPECT_EQ(ev.back().reason, Reason::STP);
  t.stp = Stp::CancelOldest;
  ev = this->input(t);  // cancels own maker, then trades with account 8
  EXPECT_EQ(count(ev, EventType::Fill), 2);
  EXPECT_EQ(ev[1].type, EventType::Cancelled);
  EXPECT_EQ(ev[1].order_id, 1u);
  EXPECT_EQ(this->x.queue(Side::Sell, 100), (std::vector<std::tuple<OrderId, Qty, Qty>>{{2, 1, 1}}));
  this->limit(8, Side::Sell, 100, 5);  // id 5
  Input b = make_limit(0, 8, Side::Buy, 100, 3);
  b.stp = Stp::CancelBoth;
  ev = this->input(b);
  EXPECT_EQ(count(ev, EventType::Cancelled), 2);
  EXPECT_EQ(this->x.resting(), 1u);  // id 5 remains
}

TYPED_TEST(Rules, ReplaceKeepsPriorityOnlyForSamePriceDecrease) {
  this->limit(1, Side::Buy, 100, 5);  // id 1
  this->limit(2, Side::Buy, 100, 5);  // id 2
  auto ev = this->input(make_replace(0, 1, 1, 100, 3));
  EXPECT_EQ(ev.back().type, EventType::Replaced);
  EXPECT_EQ(this->x.queue(Side::Buy, 100), (std::vector<std::tuple<OrderId, Qty, Qty>>{{1, 3, 3}, {2, 5, 5}}));
  this->input(make_replace(0, 1, 1, 100, 6));  // increase => back of queue
  EXPECT_EQ(this->x.queue(Side::Buy, 100), (std::vector<std::tuple<OrderId, Qty, Qty>>{{2, 5, 5}, {1, 6, 6}}));
  this->limit(3, Side::Sell, 102, 4);
  ev = this->input(make_replace(0, 1, 1, 102, 6));  // reprice through the offer => trades
  EXPECT_EQ(count(ev, EventType::Fill), 2);
  EXPECT_EQ(ev.back().type, EventType::Rested);
  EXPECT_EQ(ev.back().leaves, 2);
}

TYPED_TEST(Rules, CancelReplaceValidation) {
  this->limit(1, Side::Buy, 100, 5);
  EXPECT_EQ(this->input(make_cancel(0, 2, 1)).back().reason, Reason::NotOwner);
  EXPECT_EQ(this->input(make_cancel(0, 1, 99)).back().reason, Reason::UnknownOrder);
  EXPECT_EQ(this->input(make_replace(0, 1, 1, 100, 0)).back().reason, Reason::BadQty);
  auto ev = this->input(make_cancel(0, 1, 1));
  EXPECT_EQ(ev.back().type, EventType::Cancelled);
  EXPECT_EQ(ev.back().qty, 5);
  EXPECT_EQ(this->input(make_cancel(0, 1, 1)).back().reason, Reason::UnknownOrder);
}

TYPED_TEST(Rules, StopTriggersAndCascades) {
  this->limit(1, Side::Sell, 100, 1);
  this->limit(1, Side::Sell, 105, 10);
  Input stop = make_market(0, 2, Side::Buy, 3);
  stop.ord_type = OrdType::StopMarket;
  stop.stop_price = 100;  // triggers once something trades at >= 100
  auto ev = this->input(stop);
  EXPECT_EQ(this->x.stops(), 1u);
  EXPECT_EQ(count(ev, EventType::Triggered), 0);  // no last trade yet
  Input stop2 = stop;
  stop2.stop_price = 105;  // triggered only by the first stop's own fills at 105 (cascade)
  this->input(stop2);
  ev = this->limit(3, Side::Buy, 100, 1);  // trade at 100 => stop 1 fires, lifts 105 => stop 2 fires
  EXPECT_EQ(count(ev, EventType::Triggered), 2);
  EXPECT_EQ(this->x.stops(), 0u);
  Qty at105 = 0;
  for (auto& e : ev)
    if (e.type == EventType::Fill && e.price == 105 && e.account == 2) at105 += e.qty;
  EXPECT_EQ(at105, 6);
}

TYPED_TEST(Rules, ValidationRejects) {
  Input bad = make_limit(0, 1, Side::Buy, 0, 5);
  EXPECT_EQ(this->input(bad).back().reason, Reason::BadPrice);
  bad = make_limit(0, 1, Side::Buy, 100, 0);
  EXPECT_EQ(this->input(bad).back().reason, Reason::BadQty);
  bad = make_market(0, 1, Side::Buy, 5);
  bad.price = 100;
  EXPECT_EQ(this->input(bad).back().reason, Reason::BadPrice);
  bad = make_limit(0, 1, Side::Buy, 100, 5, Tif::IOC);
  bad.post_only = 1;
  EXPECT_EQ(this->input(bad).back().reason, Reason::BadCombo);
  bad = make_limit(0, 1, Side::Buy, 100, 5);
  bad.display_qty = 6;
  EXPECT_EQ(this->input(bad).back().reason, Reason::BadCombo);
}
