// ReferenceEngine: the obviously-correct oracle for docs/RULES.md. std::map price levels, std::list
// FIFO queues, an unordered_map index and a linear scan over stops. Written for readability, not
// speed; the optimised Engine must produce an identical event stream for every input stream.
#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "matchbox/types.hpp"

namespace matchbox {

class ReferenceEngine {
 public:
  explicit ReferenceEngine(std::uint16_t symbol = 0) : symbol_(symbol) {}

  // Processes one input and appends its events to out.
  void process(const Input& in, std::vector<Event>& out) {
    out_ = &out;
    seq_ = in.seq;
    switch (in.type) {
      case InputType::NewOrder: on_new(in); break;
      case InputType::Cancel: on_cancel(in); break;
      case InputType::Replace: on_replace(in); break;
      default: emit_reject(in, Reason::BadCombo); return;
    }
    run_triggers();
  }

  [[nodiscard]] std::optional<Price> best(Side s) const {
    if (s == Side::Buy) return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
  }
  [[nodiscard]] std::size_t resting() const { return index_.size(); }
  [[nodiscard]] std::size_t stops() const { return stops_.size(); }

  // Levels best-first: (price, total leaves, visible, order count).
  struct LevelView {
    Price price;
    Qty leaves;
    Qty visible;
    std::size_t orders;
    friend bool operator==(const LevelView&, const LevelView&) = default;
  };
  [[nodiscard]] std::vector<LevelView> levels(Side s) const {
    std::vector<LevelView> v;
    auto add = [&](const auto& m) {
      for (const auto& [px, q] : m) {
        LevelView lv{px, 0, 0, q.size()};
        for (const auto& o : q) {
          lv.leaves += o.leaves;
          lv.visible += o.visible;
        }
        v.push_back(lv);
      }
    };
    if (s == Side::Buy) add(bids_);
    else add(asks_);
    return v;
  }
  // Queue at a level: (id, leaves, visible) in time priority.
  [[nodiscard]] std::vector<std::tuple<OrderId, Qty, Qty>> queue(Side s, Price px) const {
    std::vector<std::tuple<OrderId, Qty, Qty>> v;
    auto get = [&](const auto& m) {
      auto it = m.find(px);
      if (it != m.end())
        for (const auto& o : it->second) v.emplace_back(o.id, o.leaves, o.visible);
    };
    if (s == Side::Buy) get(bids_);
    else get(asks_);
    return v;
  }

 private:
  struct Order {
    OrderId id;
    Account account;
    Side side;
    OrdType type;
    Tif tif;
    Stp stp;
    bool post_only;
    Price price;
    Price stop;
    Qty leaves;
    Qty visible;
    Qty display;
    std::uint64_t tag;
  };
  using Queue = std::list<Order>;
  struct Loc {
    Side side;
    Price price;
    Queue::iterator it;
  };

  // ---- events
  Event base(EventType t, const Order& o) const {
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
  void emit(const Event& e) { out_->push_back(e); }
  void emit_reject(const Input& in, Reason r) {
    Event e{};
    e.seq = seq_;
    e.type = EventType::Rejected;
    e.side = in.side;
    e.reason = r;
    e.symbol = symbol_;
    e.account = in.account;
    e.order_id = in.type == InputType::NewOrder ? in.seq : in.order_id;
    e.client_tag = in.client_tag;
    emit(e);
  }
  void emit_cancelled(const Order& o, Reason r, Qty q) {
    Event e = base(EventType::Cancelled, o);
    e.reason = r;
    e.qty = q;
    e.leaves = 0;
    e.price = o.price;
    emit(e);
  }

  // ---- validation
  static Reason validate(const Input& in) {
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
    if (!limitish && in.tif == Tif::GTC) return Reason::BadCombo;  // market orders cannot rest
    if (in.post_only && (in.ord_type != OrdType::Limit || in.tif != Tif::GTC)) return Reason::BadCombo;
    if (in.display_qty > 0 && (!limitish || in.tif != Tif::GTC || in.display_qty > in.qty))
      return Reason::BadCombo;
    return Reason::None;
  }

  // ---- matching helpers
  bool marketable(const Order& o) const {
    if (o.side == Side::Buy) {
      if (asks_.empty()) return false;
      return o.type == OrdType::Market || asks_.begin()->first <= o.price;
    }
    if (bids_.empty()) return false;
    return o.type == OrdType::Market || bids_.begin()->first >= o.price;
  }
  bool price_ok(const Order& o, Price level) const {
    if (o.type == OrdType::Market) return true;
    return o.side == Side::Buy ? level <= o.price : level >= o.price;
  }

  bool fok_fillable(const Order& o) const {
    Qty avail = 0;
    auto scan = [&](const auto& book) {
      for (const auto& [px, q] : book) {
        if (!price_ok(o, px)) return false;
        for (const auto& m : q) {
          if (o.stp != Stp::None && m.account == o.account) return false;
          avail += m.leaves;
          if (avail >= o.leaves) return true;
        }
      }
      return false;
    };
    return o.side == Side::Buy ? scan(asks_) : scan(bids_);
  }

  void remove_resting(OrderId id) {
    auto it = index_.find(id);
    const Loc loc = it->second;
    if (loc.side == Side::Buy) {
      auto lv = bids_.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) bids_.erase(lv);
    } else {
      auto lv = asks_.find(loc.price);
      lv->second.erase(loc.it);
      if (lv->second.empty()) asks_.erase(lv);
    }
    index_.erase(it);
  }

  void rest(Order o) {
    o.visible = o.display > 0 ? std::min(o.display, o.leaves) : o.leaves;
    Queue& q = o.side == Side::Buy ? bids_[o.price] : asks_[o.price];
    q.push_back(o);
    index_[o.id] = Loc{o.side, o.price, std::prev(q.end())};
    Event e = base(EventType::Rested, o);
    e.price = o.price;
    e.qty = o.visible;
    e.leaves = o.leaves;
    emit(e);
  }

  // Match taker t against the opposite side. Returns false if the taker was cancelled by STP.
  bool match(Order& t) {
    while (t.leaves > 0) {
      const bool buy = t.side == Side::Buy;
      if (buy ? asks_.empty() : bids_.empty()) break;
      const Price lvl_px = buy ? asks_.begin()->first : bids_.begin()->first;
      if (!price_ok(t, lvl_px)) break;
      Queue& q = buy ? asks_.begin()->second : bids_.begin()->second;
      Order& m = q.front();
      if (t.stp != Stp::None && m.account == t.account) {
        if (t.stp == Stp::CancelOldest || t.stp == Stp::CancelBoth) {
          const Order mc = m;
          remove_resting(mc.id);
          emit_cancelled(mc, Reason::STP, mc.leaves);
        }
        if (t.stp == Stp::CancelNewest || t.stp == Stp::CancelBoth) {
          emit_cancelled(t, Reason::STP, t.leaves);
          t.leaves = 0;
          return false;
        }
        continue;
      }
      const Qty fq = std::min(t.leaves, m.visible);
      t.leaves -= fq;
      m.leaves -= fq;
      m.visible -= fq;
      last_ = lvl_px;
      Event tf = base(EventType::Fill, t);
      tf.other_id = m.id;
      tf.price = lvl_px;
      tf.qty = fq;
      tf.leaves = t.leaves;
      emit(tf);
      Event mf = base(EventType::Fill, m);
      mf.other_id = t.id;
      mf.price = lvl_px;
      mf.qty = fq;
      mf.leaves = m.leaves;
      emit(mf);
      if (m.visible == 0) {
        if (m.leaves > 0) {
          // Iceberg refill: new visible slice goes to the back of the level.
          Order moved = m;
          moved.visible = std::min(moved.display, moved.leaves);
          q.pop_front();
          q.push_back(moved);
          index_[moved.id].it = std::prev(q.end());
        } else {
          remove_resting(m.id);
        }
      }
    }
    return true;
  }

  void execute(Order t) {
    if (t.tif == Tif::FOK && !fok_fillable(t)) {
      emit_cancelled(t, Reason::FOK, t.leaves);
      return;
    }
    if (!match(t)) return;
    if (t.leaves == 0) return;
    if (t.type == OrdType::Limit && t.tif == Tif::GTC) rest(t);
    else emit_cancelled(t, Reason::IOC, t.leaves);
  }

  static Order from_input(const Input& in) {
    return Order{in.seq, in.account, in.side, in.ord_type, in.tif, in.stp, in.post_only != 0,
                 in.price, in.stop_price, in.qty, 0, in.display_qty, in.client_tag};
  }

  void on_new(const Input& in) {
    if (const Reason r = validate(in); r != Reason::None) {
      emit_reject(in, r);
      return;
    }
    Order o = from_input(in);
    if (o.post_only && marketable(o)) {
      emit_reject(in, Reason::PostOnlyWouldCross);
      return;
    }
    Event a = base(EventType::Accepted, o);
    a.price = o.price;
    a.qty = o.leaves;
    a.leaves = o.leaves;
    emit(a);
    if (o.type == OrdType::StopLimit || o.type == OrdType::StopMarket) {
      stops_.push_back(o);  // triggers (incl. immediate ones) are evaluated after the input
      return;
    }
    execute(o);
  }

  void on_cancel(const Input& in) {
    if (auto it = index_.find(in.order_id); it != index_.end()) {
      const Order o = *it->second.it;
      if (o.account != in.account) {
        emit_reject(in, Reason::NotOwner);
        return;
      }
      remove_resting(o.id);
      emit_cancelled(o, Reason::User, o.leaves);
      return;
    }
    for (auto it = stops_.begin(); it != stops_.end(); ++it)
      if (it->id == in.order_id) {
        if (it->account != in.account) {
          emit_reject(in, Reason::NotOwner);
          return;
        }
        const Order o = *it;
        stops_.erase(it);
        emit_cancelled(o, Reason::User, o.leaves);
        return;
      }
    emit_reject(in, Reason::UnknownOrder);
  }

  void on_replace(const Input& in) {
    for (const auto& s : stops_)
      if (s.id == in.order_id) {
        emit_reject(in, s.account == in.account ? Reason::StopReplace : Reason::NotOwner);
        return;
      }
    auto it = index_.find(in.order_id);
    if (it == index_.end()) {
      emit_reject(in, Reason::UnknownOrder);
      return;
    }
    Order& cur = *it->second.it;
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
      // Reduce in place: priority kept. Iceberg reserve shrinks first, then the visible slice.
      Qty cut = cur.leaves - in.qty;
      const Qty reserve = cur.leaves - cur.visible;
      const Qty from_reserve = std::min(cut, reserve);
      cut -= from_reserve;
      cur.visible -= cut;
      cur.leaves = in.qty;
      Event e = base(EventType::Replaced, cur);
      e.price = cur.price;
      e.qty = cur.leaves;
      e.leaves = cur.leaves;
      emit(e);
      return;
    }
    Order o = cur;
    o.price = in.price;
    o.leaves = in.qty;
    if (o.post_only && marketable(o)) {
      emit_reject(in, Reason::PostOnlyWouldCross);
      return;
    }
    remove_resting(o.id);
    Event e = base(EventType::Replaced, o);
    e.price = o.price;
    e.qty = o.leaves;
    e.leaves = o.leaves;
    emit(e);
    execute(o);
  }

  bool triggered(const Order& s) const {
    if (!last_) return false;
    return s.side == Side::Buy ? *last_ >= s.stop : *last_ <= s.stop;
  }

  void run_triggers() {
    for (;;) {
      // Select one qualifying stop: buys before sells; buys by lowest stop, sells by highest;
      // ties by id. Re-evaluated after every triggered order (cascade).
      auto pick = stops_.end();
      for (auto it = stops_.begin(); it != stops_.end(); ++it) {
        if (!triggered(*it)) continue;
        if (pick == stops_.end()) {
          pick = it;
          continue;
        }
        auto key = [](const Order& o) {
          return std::make_tuple(o.side == Side::Buy ? 0 : 1, o.side == Side::Buy ? o.stop : -o.stop, o.id);
        };
        if (key(*it) < key(*pick)) pick = it;
      }
      if (pick == stops_.end()) return;
      Order o = *pick;
      stops_.erase(pick);
      o.type = o.type == OrdType::StopMarket ? OrdType::Market : OrdType::Limit;
      if (o.type == OrdType::Market) o.tif = o.tif == Tif::FOK ? Tif::FOK : Tif::IOC;
      Event e = base(EventType::Triggered, o);
      e.price = *last_;
      e.qty = o.leaves;
      e.leaves = o.leaves;
      emit(e);
      execute(o);
    }
  }

  std::uint16_t symbol_;
  std::uint64_t seq_ = 0;
  std::vector<Event>* out_ = nullptr;
  std::map<Price, Queue, std::greater<>> bids_;
  std::map<Price, Queue, std::less<>> asks_;
  std::unordered_map<OrderId, Loc> index_;
  std::vector<Order> stops_;
  std::optional<Price> last_;
};

}  // namespace matchbox
