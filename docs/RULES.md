# matchbox matching rules

Both engines (`Engine`, optimised; `ReferenceEngine`, naive) implement exactly these rules, and the
differential tests compare their event streams event-for-event.

## Inputs
Every input carries a sequence number assigned by the sequencer. A new order's id **is** its
sequence number, so ids are unique and increasing.

| input | fields |
|---|---|
| `NewOrder` | account, side, type (Limit, Market, StopLimit, StopMarket), TIF (GTC, IOC, FOK), price, stop price, qty, display qty (iceberg if > 0), post-only, STP mode, client tag |
| `Cancel` | order id, account |
| `Replace` | order id, account, new price, new leaves qty |

## Validation (→ `Reject`, no other effect)
- qty ≤ 0; Limit/StopLimit price ≤ 0; Market/StopMarket with a price; stop types with stop price ≤ 0.
- post-only with Market, IOC or FOK; iceberg (display > 0) with Market, IOC or FOK, or display > qty.
- Cancel/Replace of an unknown order, or one owned by a different account; Replace with qty ≤ 0 or
  price ≤ 0; Replace of a stop order (cancel and re-enter instead).

## Matching (price-time priority)
An incoming order is the **taker**. While it has quantity left and the opposite best level is
marketable (any price for Market; `ask ≤ limit` for a buy, `bid ≥ limit` for a sell), it matches the
**front** order of the best level:

1. **Self-trade prevention** (maker and taker share an account, and taker STP ≠ None):
   - CancelNewest: cancel the taker's remainder (`Cancelled`, reason STP) and stop.
   - CancelOldest: cancel the maker entirely (`Cancelled`, reason STP) and continue.
   - CancelBoth: cancel the maker and the taker's remainder, and stop.
2. Trade `q = min(taker leaves, maker visible)` at the **maker's price** (no trade-through: the
   taker always gets the resting price). Emit `Fill`.
3. If the maker's visible quantity reaches 0: an iceberg with reserve refills `min(display,
   reserve)` and moves to the **back** of its level (new time priority); otherwise the maker is
   removed.

Each fill sets the last trade price.

## After matching
- GTC Limit with quantity left: rests at its price, at the back of the level. Iceberg: visible =
  min(display, leaves), the rest is reserve.
- IOC, or Market, with quantity left: `Cancelled` (reason IOC).
- **FOK**: before any matching, the engine simulates the match. If the full quantity cannot be
  filled at acceptable prices, or a self-trade would be met first (any STP mode), the order is
  `Cancelled` (reason FOK) with **no** fills and no side effects. Icebergs count at their full
  (visible + reserve) size, because the taker keeps matching a level until it is empty.
- **Post-only**: if the order would be marketable on arrival it is rejected (`post_only_would_cross`).
  Otherwise it rests.

## Event order for a NewOrder
`Accepted` → zero or more (`Fill` | STP `Cancelled` of a maker) → at most one terminal event for the
taker's remainder (`Cancelled`) or `Rested`.

## Stops
Stop orders rest in a stop book (not visible, never matched) until triggered:
- buy stop: last trade price ≥ stop price; sell stop: last trade price ≤ stop price.
- If the condition already holds on arrival (and a last trade exists), the stop triggers immediately.

Triggers are evaluated **after an input has been fully processed**. Triggered stops convert to
Market (StopMarket) or Limit at their price (StopLimit, TIF preserved) and are processed one at a
time in a deterministic order: buy stops by stop price ascending, then sell stops by stop price
descending, each tie broken by id. Every converted order emits `Triggered` before its own events.
Fills from triggered orders can trigger further stops: the cascade repeats until no stop
qualifies. A converted stop keeps its original id.

## Cancel / Replace
- Cancel removes a resting or stop order: `Cancelled` (reason user, remaining = leaves).
- Replace to the **same price with leaves ≤ current leaves** reduces in place and **keeps time
  priority** (iceberg: reserve first, then visible). Emits `Replaced`.
- Any other replace (price change, or quantity increase) removes the order and re-enters it with
  the same id as a new limit at the new price/qty. It **loses priority**, and **may match**
  immediately (fills follow `Replaced`). A post-only order replaced to a marketable price is
  rejected and stays unchanged.

## Conservation invariants (property-tested)
For every order: `filled + leaves + cancelled_qty == original qty` (replace re-bases the
original). For every fill: one buyer and one seller; maker price used; for limit takers, the price
is within the limit. The book is never crossed after an input completes. Every resting order's
level aggregate equals the sum of its orders.
