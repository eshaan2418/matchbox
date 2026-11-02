#include "matchbox/gen.hpp"

namespace matchbox {

Gen::Gen(const GenConfig& c) : c_(c), mid_(c.mid) {
  std::uint64_t x = c.seed;
  for (auto& w : s_) {
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    w = z ^ (z >> 31);
  }
  if (c_.realistic) {
    c_.p_market = 0.03;
    c_.p_ioc = 0.10;
    c_.p_fok = 0.01;
    c_.p_post = 0.10;
    c_.p_iceberg = 0.02;
    c_.p_stop = 0.005;
    c_.p_cancel = 0.30;
    c_.p_replace = 0.10;
    c_.p_stp = 0.05;
    c_.p_invalid = 0;
  }
}

std::uint64_t Gen::rnd() {
  auto rotl = [](std::uint64_t v, int k) { return (v << k) | (v >> (64 - k)); };
  const std::uint64_t r = rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return r;
}
double Gen::uni() { return static_cast<double>(rnd() >> 11) * 0x1.0p-53; }
std::uint64_t Gen::below(std::uint64_t n) {
  __extension__ typedef unsigned __int128 u128;
  return static_cast<std::uint64_t>((static_cast<u128>(rnd()) * n) >> 64);
}

Input Gen::next() {
  Input in{};
  in.seq = ++seq_;
  in.account = static_cast<Account>(1 + below(c_.accounts));
  in.client_tag = rnd();
  if (below(64) == 0) mid_ += static_cast<Price>(below(3)) - 1;  // slow random walk
  if (mid_ < 100) mid_ = 100;
  double u = uni();
  auto pick_target = [&](bool own) -> std::pair<OrderId, Account> {
    if (ids_.empty() || below(20) == 0) return {seq_ + 1'000'000, in.account};  // unknown id
    auto t = ids_[ids_.size() - 1 - below(std::min<std::size_t>(ids_.size(), 64))];
    if (!own) return t;
    return t;
  };
  if (u < c_.p_cancel) {
    auto [id, acct] = pick_target(true);
    in.type = InputType::Cancel;
    in.order_id = id;
    in.account = below(25) == 0 ? acct + 1 : acct;  // occasionally not the owner
    return in;
  }
  u -= c_.p_cancel;
  if (u < c_.p_replace) {
    auto [id, acct] = pick_target(true);
    in.type = InputType::Replace;
    in.order_id = id;
    in.account = below(25) == 0 ? acct + 1 : acct;
    in.price = mid_ + static_cast<Price>(below(2 * static_cast<std::uint64_t>(c_.spread_ticks) + 1)) - c_.spread_ticks;
    in.qty = static_cast<Qty>(1 + below(20));
    return in;
  }
  in.type = InputType::NewOrder;
  in.side = below(2) ? Side::Sell : Side::Buy;
  in.qty = static_cast<Qty>(1 + below(20));
  // Passive bias: buys priced mostly below mid, sells above, with crossing tails.
  const Price off = static_cast<Price>(below(static_cast<std::uint64_t>(c_.spread_ticks) + 1));
  const Price aggressive = below(5) == 0 ? static_cast<Price>(below(4)) : 0;
  in.price = in.side == Side::Buy ? mid_ - off + aggressive * 3 : mid_ + off - aggressive * 3;
  in.ord_type = OrdType::Limit;
  in.tif = Tif::GTC;
  if (uni() < c_.p_stp) in.stp = static_cast<Stp>(1 + below(3));
  double v = uni();
  // Walk the order-type buckets: test v against one bucket's mass, then remove that mass before
  // the next test (same comparisons as subtracting inside each else-if condition).
  auto in_bucket = [&v](double p) {
    const bool hit = v < p;
    v -= p;
    return hit;
  };
  if (in_bucket(c_.p_market)) {
    in.ord_type = OrdType::Market;
    in.price = 0;
    in.tif = below(4) == 0 ? Tif::FOK : Tif::IOC;
  } else if (in_bucket(c_.p_ioc)) {
    in.tif = Tif::IOC;
  } else if (in_bucket(c_.p_fok)) {
    in.tif = Tif::FOK;
    in.qty = static_cast<Qty>(1 + below(60));
  } else if (in_bucket(c_.p_post)) {
    in.post_only = 1;
  } else if (in_bucket(c_.p_iceberg)) {
    in.qty = static_cast<Qty>(10 + below(50));
    in.display_qty = static_cast<Qty>(1 + below(5));
  } else if (in_bucket(c_.p_stop)) {
    const bool lim = below(2) == 0;
    in.ord_type = lim ? OrdType::StopLimit : OrdType::StopMarket;
    in.tif = lim ? Tif::GTC : Tif::IOC;
    const Price d = static_cast<Price>(1 + below(5));
    in.stop_price = in.side == Side::Buy ? mid_ + d : mid_ - d;
    if (!lim) in.price = 0;
  }
  if (uni() < c_.p_invalid) {
    switch (below(5)) {
      case 0: in.qty = 0; break;
      case 1: in.price = -5; break;
      case 2: in.post_only = 1; in.tif = Tif::IOC; break;
      case 3: in.display_qty = in.qty + 1; break;
      default: in.ord_type = OrdType::Market; break;  // market with a price
    }
  }
  ids_.push_back({in.seq, in.account});
  if (ids_.size() > 4096) ids_.erase(ids_.begin(), ids_.begin() + 2048);
  return in;
}

}  // namespace matchbox
