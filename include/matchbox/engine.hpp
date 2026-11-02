// Engine: the optimised matching engine for one instrument. Semantics are defined in
// docs/RULES.md and must match ReferenceEngine event-for-event (tests/test_differential.cpp).
//
// Data layout (all pre-sized; no heap allocation on the hot path once at working capacity):
//   orders_  pool of Order records linked into per-level FIFOs by 32-bit indices
//   levels_  pool of Level records (aggregate leaves/visible, count, FIFO head/tail)
//   bids_/asks_  sorted vectors of {price, level} with the best price at back(): adds and removes
//               near the touch touch only the end of a small array
//   ids_     open-addressing map order id -> pool index (resting and stop orders)
//   bstops_/sstops_  sorted stop books; the next candidate is always at the front
//
// Events go to a Sink template parameter (on_event(const Event&)), so emission inlines.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "matchbox/order_map.hpp"
#include "matchbox/types.hpp"

namespace matchbox {

struct EngineConfig {
  std::size_t order_capacity = 1u << 20;
  std::size_t level_capacity = 1u << 16;
  std::size_t stop_capacity = 1u << 12;
  std::uint16_t symbol = 0;
};

template <class Sink>
class Engine {
 public:
  static constexpr std::uint32_t kNil = ~std::uint32_t{0};

  explicit Engine(Sink& sink, const EngineConfig& cfg = {})
      : sink_(sink), symbol_(cfg.symbol), ids_(cfg.order_capacity * 2) {
    orders_.reserve(cfg.order_capacity);
    free_orders_.reserve(cfg.order_capacity);
    levels_.reserve(cfg.level_capacity);
    free_levels_.reserve(cfg.level_capacity);
    bids_.reserve(cfg.level_capacity / 2);
    asks_.reserve(cfg.level_capacity / 2);
    bstops_.reserve(cfg.stop_capacity);
    sstops_.reserve(cfg.stop_capacity);
  }

  void process(const Input& in) noexcept {
    seq_ = in.seq;
    switch (in.type) {
      case InputType::NewOrder: on_new(in); break;
      case InputType::Cancel: on_cancel(in); break;
      case InputType::Replace: on_replace(in); break;
      default: emit_reject(in, Reason::BadCombo); return;
    }
    if (!bstops_.empty() || !sstops_.empty()) [[unlikely]] run_triggers();
  }

  // ---- inspection (tests, snapshots)
  [[nodiscard]] bool has_best(Side s) const noexcept { return !(s == Side::Buy ? bids_ : asks_).empty(); }
  [[nodiscard]] Price best(Side s) const noexcept { return (s == Side::Buy ? bids_ : asks_).back().price; }
  [[nodiscard]] std::size_t resting() const noexcept { return ids_.size() - bstops_.size() - sstops_.size(); }
  [[nodiscard]] std::size_t stops() const noexcept { return bstops_.size() + sstops_.size(); }

  struct LevelView {
    Price price;
    Qty leaves;
    Qty visible;
    std::size_t orders;
    friend bool operator==(const LevelView&, const LevelView&) = default;
  };
  [[nodiscard]] std::vector<LevelView> levels(Side s) const {
    std::vector<LevelView> v;
    const auto& side = s == Side::Buy ? bids_ : asks_;
    for (auto it = side.rbegin(); it != side.rend(); ++it) {
      const Level& l = levels_[it->level];
      v.push_back({l.price, l.leaves, l.visible, l.count});
    }
    return v;
  }
  template <class F>
  void for_each_in_queue(Side s, Price px, F&& f) const {
    const auto& side = s == Side::Buy ? bids_ : asks_;
    for (const auto& r : side)
      if (r.price == px)
        for (std::uint32_t i = levels_[r.level].head; i != kNil; i = orders_[i].next)
          f(orders_[i].id, orders_[i].leaves, orders_[i].visible);
  }

 private:
  struct Order {
    OrderId id;
    std::uint64_t tag;
    Price price;
    Price stop;
    Qty leaves;
    Qty visible;
    Qty display;
    Account account;
    std::uint32_t level;
    std::uint32_t prev;
    std::uint32_t next;
    Side side;
    OrdType type;
    Tif tif;
    Stp stp;
    bool post_only;
  };
  struct Level {
    Price price;
    Qty leaves;
    Qty visible;
    std::uint32_t count;
    std::uint32_t head;
    std::uint32_t tail;
  };
  struct LevelRef {
    Price price;
    std::uint32_t level;
  };
  struct StopRef {
    Price key;  // buy: stop price; sell: -stop price (so both books sort ascending)
    OrderId id;
    std::uint32_t idx;
  };

  // ---- events
  Event base(EventType t, const Order& o) const noexcept {
    Event e{};
    e.seq = seq_;
    e.type = t;
    e.side = o.side;
    e.symbol = symbol_;
    e.account = o.account;
    e.order_id = o.id;
    e.client_tag = o.tag;
    return e;
  }
  void emit_reject(const Input& in, Reason r) noexcept {
    Event e{};
    e.seq = seq_;
    e.type = EventType::Rejected;
    e.side = in.side;
    e.reason = r;
    e.symbol = symbol_;
    e.account = in.account;
    e.order_id = in.type == InputType::NewOrder ? in.seq : in.order_id;
    e.client_tag = in.client_tag;
    sink_.on_event(e);
  }
  void emit_cancelled(const Order& o, Reason r, Qty q) noexcept {
    Event e = base(EventType::Cancelled, o);
    e.reason = r;
    e.qty = q;
    e.price = o.price;
    sink_.on_event(e);
  }

  static Reason validate(const Input& in) noexcept {
    const bool stop = in.ord_type == OrdType::StopLimit || in.ord_type == OrdType::StopMarket;
    const bool limitish = in.ord_type == OrdType::Limit || in.ord_type == OrdType::StopLimit;
    if (in.side != Side::Buy && in.side != Side::Sell) return Reason::BadCombo;
    if (static_cast<std::uint8_t>(in.ord_type) > 3 || static_cast<std::uint8_t>(in.tif) > 2 ||
        static_cast<std::uint8_t>(in.stp) > 3)
      return Reason::BadCombo;
    if (in.qty <= 0 || in.display_qty < 0) return Reason::BadQty;
    if (limitish && in.price <= 0) return Reason::BadPrice;
    if (!limitish && in.price != 0) return Reason::BadPrice;
    if (stop && in.stop_price <= 0) return Reason::BadStop;
    if (!stop && in.stop_price != 0) return Reason::BadStop;
    if (!limitish && in.tif == Tif::GTC) return Reason::BadCombo;
    if (in.post_only && (in.ord_type != OrdType::Limit || in.tif != Tif::GTC)) return Reason::BadCombo;
    if (in.display_qty > 0 && (!limitish || in.tif != Tif::GTC || in.display_qty > in.qty))
      return Reason::BadCombo;
    return Reason::None;
  }

  static bool price_ok(const Order& t, Price level) noexcept {
    if (t.type == OrdType::Market) return true;
    return t.side == Side::Buy ? level <= t.price : level >= t.price;
  }
  bool marketable(const Order& t) const noexcept {
    const auto& opp = t.side == Side::Buy ? asks_ : bids_;
    return !opp.empty() && price_ok(t, opp.back().price);
  }

  // ---- pools
  std::uint32_t alloc_order() {
    if (!free_orders_.empty()) {
      const std::uint32_t i = free_orders_.back();
      free_orders_.pop_back();
      return i;
    }
    orders_.emplace_back();
    return static_cast<std::uint32_t>(orders_.size() - 1);
  }
  std::uint32_t alloc_level(Price px) {
    std::uint32_t i;
    if (!free_levels_.empty()) {
      i = free_levels_.back();
      free_levels_.pop_back();
    } else {
      i = static_cast<std::uint32_t>(levels_.size());
      levels_.emplace_back();
    }
    levels_[i] = Level{px, 0, 0, 0, kNil, kNil};
    return i;
  }

  // Sides are sorted worst -> best. "better(a, b)": a is a better price than b for this side.
  static bool better(Side s, Price a, Price b) noexcept { return s == Side::Buy ? a > b : a < b; }

  std::uint32_t level_for(Side s, Price px) {
    auto& v = s == Side::Buy ? bids_ : asks_;
    std::size_t pos = v.size();
    while (pos > 0 && better(s, v[pos - 1].price, px)) --pos;
    if (pos > 0 && v[pos - 1].price == px) return v[pos - 1].level;
    const std::uint32_t li = alloc_level(px);
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), LevelRef{px, li});
    return li;
  }
  void drop_level(Side s, std::uint32_t li) {
    auto& v = s == Side::Buy ? bids_ : asks_;
    std::size_t pos = v.size();
    while (v[pos - 1].level != li) --pos;
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos - 1));
    free_levels_.push_back(li);
  }

  void link_back(std::uint32_t oi, std::uint32_t li) noexcept {
    Order& o = orders_[oi];
    Level& l = levels_[li];
    o.level = li;
    o.next = kNil;
    o.prev = l.tail;
    if (l.tail != kNil) orders_[l.tail].next = oi;
    else l.head = oi;
    l.tail = oi;
    ++l.count;
    l.leaves += o.leaves;
    l.visible += o.visible;
  }
  // Unlink from its level (level aggregates adjusted by the order's current leaves/visible).
  void unlink(std::uint32_t oi) noexcept {
    Order& o = orders_[oi];
    Level& l = levels_[o.level];
    if (o.prev != kNil) orders_[o.prev].next = o.next;
    else l.head = o.next;
    if (o.next != kNil) orders_[o.next].prev = o.prev;
    else l.tail = o.prev;
    --l.count;
    l.leaves -= o.leaves;
    l.visible -= o.visible;
  }
  void remove_resting(std::uint32_t oi) {
    const Order& o = orders_[oi];
    const std::uint32_t li = o.level;
    unlink(oi);
    if (levels_[li].count == 0) drop_level(o.side, li);
    ids_.erase(o.id);
    free_orders_.push_back(oi);
  }

  void rest(std::uint32_t oi) {
    Order& o = orders_[oi];
    o.visible = o.display > 0 ? std::min(o.display, o.leaves) : o.leaves;
    link_back(oi, level_for(o.side, o.price));
    ids_.insert(o.id, oi);
    Event e = base(EventType::Rested, o);
    e.price = o.price;
    e.qty = o.visible;
    e.leaves = o.leaves;
    sink_.on_event(e);
  }

  bool fok_fillable(const Order& t) const noexcept {
    const auto& opp = t.side == Side::Buy ? asks_ : bids_;
    Qty avail = 0;
    for (auto it = opp.rbegin(); it != opp.rend(); ++it) {
      if (!price_ok(t, it->price)) return false;
      const Level& l = levels_[it->level];
      if (t.stp == Stp::None) {
        avail += l.leaves;  // no STP: the level aggregate suffices
      } else {
        for (std::uint32_t i = l.head; i != kNil; i = orders_[i].next) {
          if (orders_[i].account == t.account) return false;
          avail += orders_[i].leaves;
          if (avail >= t.leaves) return true;
        }
      }
      if (avail >= t.leaves) return true;
    }
    return false;
  }

  // Match taker (pool index ti) against the book. Returns false if STP cancelled the taker.
  bool match(std::uint32_t ti) {
    for (;;) {
      Order& t = orders_[ti];
      if (t.leaves == 0) return true;
      auto& opp = t.side == Side::Buy ? asks_ : bids_;
      if (opp.empty()) return true;
      const LevelRef top = opp.back();
      if (!price_ok(t, top.price)) return true;
      Level& l = levels_[top.level];
      const std::uint32_t mi = l.head;
      Order& m = orders_[mi];
      if (t.stp != Stp::None && m.account == t.account) [[unlikely]] {
        if (t.stp == Stp::CancelOldest || t.stp == Stp::CancelBoth) {
          const Order mc = m;
          remove_resting(mi);
          emit_cancelled(mc, Reason::STP, mc.leaves);
        }
        if (t.stp == Stp::CancelNewest || t.stp == Stp::CancelBoth) {
          Order& tt = orders_[ti];
          emit_cancelled(tt, Reason::STP, tt.leaves);
          tt.leaves = 0;
          return false;
        }
        continue;
      }
      const Qty fq = std::min(t.leaves, m.visible);
      t.leaves -= fq;
      m.leaves -= fq;
      m.visible -= fq;
      l.leaves -= fq;
      l.visible -= fq;
      last_ = top.price;
      has_last_ = true;
      Event tf = base(EventType::Fill, t);
      tf.other_id = m.id;
      tf.price = top.price;
      tf.qty = fq;
      tf.leaves = t.leaves;
      sink_.on_event(tf);
      Event mf = base(EventType::Fill, m);
      mf.other_id = t.id;
      mf.price = top.price;
      mf.qty = fq;
      mf.leaves = m.leaves;
      sink_.on_event(mf);
      if (m.visible == 0) {
        if (m.leaves > 0) {
          // Iceberg refill: unlink, new visible slice, back of the same level (priority lost).
          unlink(mi);
          m.visible = std::min(m.display, m.leaves);
          link_back(mi, top.level);
        } else {
          remove_resting(mi);
        }
      }
    }
  }

  // Execute a taker that already sits in the pool at index ti (not in ids_ / not linked).
  void execute(std::uint32_t ti) {
    Order& t = orders_[ti];
    if (t.tif == Tif::FOK && !fok_fillable(t)) {
      emit_cancelled(t, Reason::FOK, t.leaves);
      free_orders_.push_back(ti);
      return;
    }
    if (!match(ti) || orders_[ti].leaves == 0) {
      free_orders_.push_back(ti);
      return;
    }
    Order& o = orders_[ti];
    if (o.type == OrdType::Limit && o.tif == Tif::GTC) {
      rest(ti);
    } else {
      emit_cancelled(o, Reason::IOC, o.leaves);
      free_orders_.push_back(ti);
    }
  }

  void on_new(const Input& in) {
    if (const Reason r = validate(in); r != Reason::None) [[unlikely]] {
      emit_reject(in, r);
      return;
    }
    const std::uint32_t oi = alloc_order();
    orders_[oi] = Order{in.seq, in.client_tag, in.price, in.stop_price, in.qty, 0, in.display_qty,
                        in.account, kNil, kNil, kNil, in.side, in.ord_type, in.tif, in.stp,
                        in.post_only != 0};
    Order& o = orders_[oi];
    if (o.post_only && marketable(o)) {
      emit_reject(in, Reason::PostOnlyWouldCross);
      free_orders_.push_back(oi);
      return;
    }
    Event a = base(EventType::Accepted, o);
    a.price = o.price;
    a.qty = o.leaves;
    a.leaves = o.leaves;
    sink_.on_event(a);
    if (o.type == OrdType::StopLimit || o.type == OrdType::StopMarket) [[unlikely]] {
      add_stop(oi);
      return;
    }
    execute(oi);
  }

  void on_cancel(const Input& in) {
    const std::uint32_t oi = ids_.find(in.order_id);
    if (oi == OrderMap::kNotFound) {
      emit_reject(in, Reason::UnknownOrder);
      return;
    }
    const Order o = orders_[oi];
    if (o.account != in.account) {
      emit_reject(in, Reason::NotOwner);
      return;
    }
    if (o.level == kNil) remove_stop(oi);  // stop orders are never linked into a level
    else remove_resting(oi);
    emit_cancelled(o, Reason::User, o.leaves);
  }

  void on_replace(const Input& in) {
    const std::uint32_t oi = ids_.find(in.order_id);
    if (oi == OrderMap::kNotFound) {
      emit_reject(in, Reason::UnknownOrder);
      return;
    }
    Order& cur = orders_[oi];
    if (cur.level == kNil) {
      emit_reject(in, cur.account == in.account ? Reason::StopReplace : Reason::NotOwner);
      return;
    }
    if (cur.account != in.account) {
      emit_reject(in, Reason::NotOwner);
      return;
    }
    if (in.qty <= 0) {
      emit_reject(in, Reason::BadQty);
      return;
    }
    if (in.price <= 0) {
      emit_reject(in, Reason::BadPrice);
      return;
    }
    if (in.price == cur.price && in.qty <= cur.leaves) {
      Qty cut = cur.leaves - in.qty;
      const Qty reserve = cur.leaves - cur.visible;
      const Qty from_reserve = std::min(cut, reserve);
      cut -= from_reserve;
      Level& l = levels_[cur.level];
      l.leaves -= cur.leaves - in.qty;
      l.visible -= cut;
      cur.visible -= cut;
      cur.leaves = in.qty;
      Event e = base(EventType::Replaced, cur);
      e.price = cur.price;
      e.qty = cur.leaves;
      e.leaves = cur.leaves;
      sink_.on_event(e);
      return;
    }
    Order probe = cur;
    probe.price = in.price;
    probe.leaves = in.qty;
    if (probe.post_only && marketable(probe)) {
      emit_reject(in, Reason::PostOnlyWouldCross);
      return;
    }
    // Re-enter as a new limit with the same id: unlink, reuse the pool slot.
    unlink(oi);
    if (levels_[cur.level].count == 0) drop_level(cur.side, cur.level);
    ids_.erase(cur.id);
    Order& o = orders_[oi];
    o.price = in.price;
    o.leaves = in.qty;
    o.visible = 0;
    o.level = kNil;
    Event e = base(EventType::Replaced, o);
    e.price = o.price;
    e.qty = o.leaves;
    e.leaves = o.leaves;
    sink_.on_event(e);
    execute(oi);
  }

  // ---- stops
  void add_stop(std::uint32_t oi) {
    Order& o = orders_[oi];
    o.level = kNil;
    ids_.insert(o.id, oi);
    auto& book = o.side == Side::Buy ? bstops_ : sstops_;
    const StopRef r{o.side == Side::Buy ? o.stop : -o.stop, o.id, oi};
    auto it = std::upper_bound(book.begin(), book.end(), r, [](const StopRef& a, const StopRef& b) {
      return a.key != b.key ? a.key < b.key : a.id < b.id;
    });
    book.insert(it, r);
  }
  void remove_stop(std::uint32_t oi) {
    const Order& o = orders_[oi];
    auto& book = o.side == Side::Buy ? bstops_ : sstops_;
    const StopRef r{o.side == Side::Buy ? o.stop : -o.stop, o.id, oi};
    auto it = std::lower_bound(book.begin(), book.end(), r, [](const StopRef& a, const StopRef& b) {
      return a.key != b.key ? a.key < b.key : a.id < b.id;
    });
    book.erase(it);
    ids_.erase(o.id);
    free_orders_.push_back(oi);
  }

  void run_triggers() {
    while (has_last_) {
      std::uint32_t oi;
      if (!bstops_.empty() && last_ >= bstops_.front().key) {
        oi = bstops_.front().idx;
        bstops_.erase(bstops_.begin());
      } else if (!sstops_.empty() && last_ <= -sstops_.front().key) {
        oi = sstops_.front().idx;
        sstops_.erase(sstops_.begin());
      } else {
        return;
      }
      Order& o = orders_[oi];
      ids_.erase(o.id);
      o.type = o.type == OrdType::StopMarket ? OrdType::Market : OrdType::Limit;
      if (o.type == OrdType::Market) o.tif = o.tif == Tif::FOK ? Tif::FOK : Tif::IOC;
      Event e = base(EventType::Triggered, o);
      e.price = last_;
      e.qty = o.leaves;
      e.leaves = o.leaves;
      sink_.on_event(e);
      execute(oi);
    }
  }

  Sink& sink_;
  std::uint16_t symbol_;
  std::uint64_t seq_ = 0;
  OrderMap ids_;
  std::vector<Order> orders_;
  std::vector<std::uint32_t> free_orders_;
  std::vector<Level> levels_;
  std::vector<std::uint32_t> free_levels_;
  std::vector<LevelRef> bids_, asks_;
  std::vector<StopRef> bstops_, sstops_;
  Price last_ = 0;
  bool has_last_ = false;
};

}  // namespace matchbox
