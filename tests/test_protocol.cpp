#include <gtest/gtest.h>

#include "matchbox/gen.hpp"
#include "matchbox/net.hpp"

using namespace matchbox;

TEST(Protocol, RoundTripsAllMessages) {
  std::vector<unsigned char> b;
  Input no{};
  no.client_tag = 0xABCDEF;
  no.side = Side::Sell;
  no.ord_type = OrdType::StopLimit;
  no.tif = Tif::GTC;
  no.stp = Stp::CancelBoth;
  no.post_only = 0;
  no.price = 10'050;
  no.stop_price = 10'040;
  no.qty = 77;
  no.display_qty = 7;
  put_logon(b, 42);
  put_new_order(b, no);
  put_cancel(b, 5, 123);
  put_replace(b, 6, 124, 9'999, 3);
  Event ev{};
  ev.type = EventType::Fill;
  ev.order_id = 9;
  ev.qty = 4;
  put_event(b, ev);
  std::size_t off = 0;
  std::vector<Decoded> got;
  while (off < b.size()) {
    const std::size_t len = frame_length(b.data() + off, b.size() - off);
    ASSERT_GT(len, 0u);
    ASSERT_NE(len, SIZE_MAX);
    Decoded d;
    ASSERT_EQ(decode_frame(b.data() + off, len, d), DecodeStatus::Ok);
    got.push_back(d);
    off += len;
  }
  ASSERT_EQ(got.size(), 5u);
  EXPECT_EQ(got[0].account, 42u);
  const Input& n = got[1].input;
  EXPECT_EQ(n.client_tag, no.client_tag);
  EXPECT_EQ(n.side, no.side);
  EXPECT_EQ(n.ord_type, no.ord_type);
  EXPECT_EQ(n.stp, no.stp);
  EXPECT_EQ(n.price, no.price);
  EXPECT_EQ(n.stop_price, no.stop_price);
  EXPECT_EQ(n.qty, no.qty);
  EXPECT_EQ(n.display_qty, no.display_qty);
  EXPECT_EQ(got[2].input.order_id, 123u);
  EXPECT_EQ(got[3].input.price, 9'999);
  EXPECT_EQ(got[3].input.qty, 3);
  EXPECT_TRUE(got[4].event == ev);
}

TEST(Protocol, FramerHandlesPartialAndBadHeaders) {
  std::vector<unsigned char> b;
  put_cancel(b, 1, 2);
  for (std::size_t cut = 0; cut < b.size(); ++cut) EXPECT_EQ(frame_length(b.data(), cut), 0u);
  EXPECT_EQ(frame_length(b.data(), b.size()), b.size());
  unsigned char bad[3] = {1, 0, 2};  // length 1 < header
  EXPECT_EQ(frame_length(bad, 3), SIZE_MAX);
  unsigned char big[3] = {0xFF, 0xFF, 2};
  EXPECT_EQ(frame_length(big, 3), SIZE_MAX);
  Decoded d;
  unsigned char wrong[5] = {5, 0, 3, 0, 0};  // cancel with a 2-byte payload
  EXPECT_EQ(decode_frame(wrong, 5, d), DecodeStatus::BadLength);
  unsigned char unknown[3] = {3, 0, 77};
  EXPECT_EQ(decode_frame(unknown, 3, d), DecodeStatus::BadType);
}
