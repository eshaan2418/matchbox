// Core types: inputs (orders, cancels, replaces) and output events. All fixed-size PODs so they
// can be journaled, sent over the wire and compared byte-for-byte. See docs/RULES.md.
#pragma once

#include <cstdint>
#include <cstring>

namespace matchbox {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks
using Qty = std::int64_t;
using Account = std::uint32_t;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrdType : std::uint8_t { Limit = 0, Market = 1, StopLimit = 2, StopMarket = 3 };
enum class Tif : std::uint8_t { GTC = 0, IOC = 1, FOK = 2 };
enum class Stp : std::uint8_t { None = 0, CancelNewest = 1, CancelOldest = 2, CancelBoth = 3 };

enum class InputType : std::uint8_t { NewOrder = 1, Cancel = 2, Replace = 3 };

// One sequenced input. 72 bytes, trivially copyable, no implicit padding.
struct Input {
  std::uint64_t seq;  // assigned by the sequencer; a NewOrder's id == seq
  InputType type;
  Side side;
  OrdType ord_type;
  Tif tif;
  Stp stp;
  std::uint8_t post_only;
  std::uint16_t symbol;
  Account account;
  std::uint32_t pad0;
  OrderId order_id;  // Cancel/Replace target
  Price price;
  Price stop_price;
  Qty qty;
  Qty display_qty;  // iceberg if > 0 (NewOrder)
  std::uint64_t client_tag;
};

enum class EventType : std::uint8_t {
  Accepted = 1,
  Rejected = 2,
  Fill = 3,
  Rested = 4,
  Cancelled = 5,
  Replaced = 6,
  Triggered = 7,
};

enum class Reason : std::uint8_t {
  None = 0,
  User = 1,
  IOC = 2,
  FOK = 3,
  STP = 4,
  BadQty = 10,
  BadPrice = 11,
  BadStop = 12,
  BadCombo = 13,
  PostOnlyWouldCross = 14,
  UnknownOrder = 15,
  NotOwner = 16,
  StopReplace = 17,
};

// One output event. 72 bytes, no implicit padding (compared with memcmp).
struct Event {
  std::uint64_t seq;  // input that caused it
  EventType type;
  Side side;          // side of order_id
  Reason reason;
  std::uint8_t pad0;
  std::uint16_t symbol;
  std::uint16_t pad1;
  Account account;    // account of order_id
  std::uint32_t pad2;
  OrderId order_id;   // subject order (taker for fills)
  OrderId other_id;   // maker id for fills
  Price price;        // fill price / resting price / replace price
  Qty qty;            // fill qty / rested visible qty / cancelled qty / replace leaves
  Qty leaves;         // subject order leaves after the event
  std::uint64_t client_tag;
  friend bool operator==(const Event& a, const Event& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
};
static_assert(sizeof(Input) == 72, "Input layout");
static_assert(sizeof(Event) == 72, "Event layout");

inline Input make_limit(std::uint64_t seq, Account acct, Side s, Price px, Qty q, Tif tif = Tif::GTC) {
  Input in{};
  in.seq = seq;
  in.type = InputType::NewOrder;
  in.side = s;
  in.ord_type = OrdType::Limit;
  in.tif = tif;
  in.account = acct;
  in.price = px;
  in.qty = q;
  return in;
}
inline Input make_market(std::uint64_t seq, Account acct, Side s, Qty q) {
  Input in{};
  in.seq = seq;
  in.type = InputType::NewOrder;
  in.side = s;
  in.ord_type = OrdType::Market;
  in.tif = Tif::IOC;
  in.account = acct;
  in.qty = q;
  return in;
}
inline Input make_cancel(std::uint64_t seq, Account acct, OrderId id) {
  Input in{};
  in.seq = seq;
  in.type = InputType::Cancel;
  in.account = acct;
  in.order_id = id;
  return in;
}
inline Input make_replace(std::uint64_t seq, Account acct, OrderId id, Price px, Qty q) {
  Input in{};
  in.seq = seq;
  in.type = InputType::Replace;
  in.account = acct;
  in.order_id = id;
  in.price = px;
  in.qty = q;
  return in;
}

// FNV-1a over raw bytes; used for event-stream hashes (determinism checks).
inline std::uint64_t fnv1a(std::uint64_t h, const void* p, std::size_t n) {
  const auto* b = static_cast<const unsigned char*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}
inline constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;

}  // namespace matchbox
