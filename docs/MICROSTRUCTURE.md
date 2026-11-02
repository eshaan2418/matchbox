# Market microstructure behind matchbox

## Continuous double auction and price-time priority
Exchanges match orders continuously. A resting order is a **maker**; an incoming order that
executes against it is a **taker**. Priority is by price first (best bid = highest, best offer =
lowest), then by time of arrival at that price (FIFO). Every trade happens at the **maker's
price**. This means the taker never trades through a better resting price, and price improvement
accrues to the taker: a buy limit at 102 that meets an offer at 100 pays 100.

Pro-rata allocation (common in some futures markets) and price-size-time priority are the main
alternatives. matchbox implements pure price-time, the model used by most equities exchanges.

## Order types and why each exists

| type | behaviour | who uses it |
|---|---|---|
| limit (GTC/day) | trades up to its price, rest rests | market makers quoting, patient traders |
| market | trades at any price until filled, rest cancelled | urgent liquidity takers; dangerous in thin books (no protection here) |
| IOC | limit that never rests | takers that must not leave a signal on the book |
| FOK | all or nothing, immediately | strategies needing exact size, e.g. one leg of a hedge |
| post-only | rejected if it would take | makers that only want maker rebates and never pay taker fees |
| iceberg | shows a slice, refills from a hidden reserve | large orders hiding size to limit information leakage |
| stop / stop-limit | dormant until the last trade crosses the stop price | stop-losses, breakout entries |

### Design choices that are venue-specific
- **Iceberg refill priority**: each refill joins the back of the queue. Displayed size keeps
  priority; hidden size does not. This is what most venues do, because otherwise icebergs would
  dominate the queue.
- **Post-only**: matchbox rejects. Some venues instead re-price the order one tick behind the
  touch ("post-only slide").
- **Replace priority**: reducing quantity at the same price keeps time priority, because the
  order only becomes less aggressive. Any price change or size increase loses it. This is the
  near-universal rule; without it, a trader could hold the front of the queue with a small order
  and then increase it.
- **Stops** trigger on the last trade price, not on the BBO, and are converted in a deterministic
  order: buys before sells, by stop price, then id. A stop cascade, where fills from triggered
  stops trigger further stops, is the mechanism behind some flash crashes. Real venues add price
  bands and volatility halts to break it.

## Self-trade prevention
An account trading with itself creates wash trades. These are prohibited (they create false
volume) and can also be accidental, e.g. two strategies of one firm on opposite sides. Exchanges
offer STP modes:
- cancel newest (the incoming taker);
- cancel oldest (the resting maker);
- cancel both.

STP interacts with FOK. matchbox treats any would-be self-trade as making the FOK unfillable, and
cancels the FOK with no side effects. Cancelling the resting order as a side effect of a FOK that
ultimately fails would surprise the user.

## Sequencing, determinism and the journal
An exchange's matching engine must be a deterministic function of its input sequence. The
sequencer is the single point that decides that order, and everything downstream (matching,
market data, clearing) can be rebuilt from the sequenced log. That is what makes:
- **recovery** possible: replay the journal after a crash;
- **replication** possible: run hot standbys on the same log;
- **audits** possible: any dispute can be replayed exactly.

matchbox's core thread is that sequencer. The journal's event hash matching the live run's hash
proves the determinism.

## Latency in an exchange
Order-to-ack latency is what a participant experiences: wire → gateway → sequencer → engine →
gateway → wire. Real venues quote tens of microseconds (equities) down to single-digit
microseconds (some futures and FX venues, FPGA-assisted gateways). matchbox reports two numbers:
the engine alone (nanoseconds) and the full loopback TCP path (tens of microseconds on a laptop,
dominated by kernel network stack and syscalls).
