// mbload: open-loop load generator for matchbox over TCP.
//
// Each connection logs on with its own account and sends a realistic mix (passive GTC limits,
// aggressive IOCs, cancels of its own resting orders) on a fixed schedule. Latency is measured
// from the *scheduled* send time to the Accepted/Rejected event (order -> ack), so a stalled
// server cannot hide queueing delay by slowing the generator down (no coordinated omission).
#include <sys/socket.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "matchbox/latency.hpp"
#include "matchbox/net.hpp"

using namespace matchbox;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}

struct Conn {
  int fd;
  Account account;
  std::vector<unsigned char> in, out;
  std::uint64_t next_tag = 1;
  std::unordered_map<std::uint64_t, std::uint64_t> pending;  // tag -> scheduled send tick
  std::vector<OrderId> resting;
  std::uint64_t rng;
  std::uint64_t acks = 0, fills = 0, rejects = 0;
};

std::uint64_t xr(std::uint64_t& s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}
}  // namespace

int main(int argc, char** argv) {
  const std::string host = arg(argc, argv, "--host", "127.0.0.1");
  const auto port = static_cast<std::uint16_t>(std::atoi(arg(argc, argv, "--port", "9000")));
  const int nconn = std::atoi(arg(argc, argv, "--connections", "8"));
  const double rate = std::atof(arg(argc, argv, "--rate", "100000"));  // requests/s total
  const double duration = std::atof(arg(argc, argv, "--duration", "10"));
  const double warmup = std::atof(arg(argc, argv, "--warmup", "2"));
  const char* hist_out = arg(argc, argv, "--hist-out", nullptr);

  const double ns_tick = Clock::ns_per_tick();
  std::vector<Conn> conns(static_cast<std::size_t>(nconn));
  Poller poller;
  std::unordered_map<int, std::size_t> byfd;
  for (int i = 0; i < nconn; ++i) {
    Conn& c = conns[static_cast<std::size_t>(i)];
    c.fd = connect_tcp(host, port);
    if (c.fd < 0) {
      std::fprintf(stderr, "connect failed\n");
      return 1;
    }
    c.account = static_cast<Account>(1000 + i);
    c.rng = 0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(i + 1);
    put_logon(c.out, c.account);
    poller.add(c.fd);
    byfd[c.fd] = static_cast<std::size_t>(i);
  }
  Histogram hist;
  const auto interval = static_cast<std::uint64_t>(1e9 / rate / ns_tick);  // ticks between sends
  const std::uint64_t t0 = Clock::now();
  const std::uint64_t t_measure = t0 + static_cast<std::uint64_t>(warmup * 1e9 / ns_tick);
  const std::uint64_t t_end = t_measure + static_cast<std::uint64_t>(duration * 1e9 / ns_tick);
  std::uint64_t next_send = t0 + static_cast<std::uint64_t>(0.2e9 / ns_tick);  // let logons land
  std::uint64_t sent = 0, sent_measured = 0, measured = 0, lost_conn = 0;
  std::size_t rr = 0;
  std::vector<Poller::Ready> ready;
  const Price mid = 10'000;
  auto flush = [&](Conn& c) {
    std::size_t off = 0;
    while (off < c.out.size()) {
      const ssize_t w = ::send(c.fd, c.out.data() + off, c.out.size() - off, 0);
      if (w <= 0) break;
      off += static_cast<std::size_t>(w);
    }
    c.out.erase(c.out.begin(), c.out.begin() + static_cast<std::ptrdiff_t>(off));
  };
  for (;;) {
    std::uint64_t now = Clock::now();
    if (now >= t_end + static_cast<std::uint64_t>(0.5e9 / ns_tick)) break;
    // Send everything that is due (catch up after stalls: open loop).
    while (now >= next_send && next_send < t_end) {
      Conn& c = conns[rr++ % conns.size()];
      const std::uint64_t r = xr(c.rng);
      const std::uint64_t tag = c.next_tag++;
      if (r % 10 < 2 && !c.resting.empty()) {
        const OrderId id = c.resting.back();
        c.resting.pop_back();
        put_cancel(c.out, tag, id);
      } else {
        Input in{};
        in.client_tag = tag;
        in.side = (r >> 8) & 1 ? Side::Sell : Side::Buy;
        in.ord_type = OrdType::Limit;
        in.qty = static_cast<Qty>(1 + (r >> 16) % 10);
        const Price off = static_cast<Price>((r >> 24) % 20);
        if (r % 10 < 4) {  // aggressive IOC through the touch
          in.tif = Tif::IOC;
          in.price = in.side == Side::Buy ? mid + 5 : mid - 5;
        } else {  // passive GTC
          in.tif = Tif::GTC;
          in.price = in.side == Side::Buy ? mid - 1 - off : mid + 1 + off;
        }
        put_new_order(c.out, in);
        c.pending[tag] = next_send;
        if (next_send >= t_measure) ++sent_measured;
      }
      ++sent;
      next_send += interval;
    }
    for (auto& c : conns)
      if (!c.out.empty()) flush(c);
    poller.wait(ready, 0);
    now = Clock::now();
    for (const auto& rd : ready) {
      Conn& c = conns[byfd[rd.fd]];
      unsigned char buf[65536];
      for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
        if (n > 0) {
          c.in.insert(c.in.end(), buf, buf + n);
          continue;
        }
        if (n == 0) ++lost_conn;
        break;
      }
      std::size_t off = 0;
      for (;;) {
        const std::size_t len = frame_length(c.in.data() + off, c.in.size() - off);
        if (len == 0 || len == SIZE_MAX) break;
        Decoded d;
        if (decode_frame(c.in.data() + off, len, d) == DecodeStatus::Ok && d.type == MsgType::EventMsg) {
          const Event& e = d.event;
          if (e.account == c.account) {
            if (e.type == EventType::Accepted || e.type == EventType::Rejected) {
              auto it = c.pending.find(e.client_tag);
              if (it != c.pending.end()) {
                if (it->second >= t_measure && it->second < t_end) {
                  hist.record(now > it->second ? now - it->second : 0);
                  ++measured;
                }
                c.pending.erase(it);
              }
              if (e.type == EventType::Accepted) ++c.acks;
              else ++c.rejects;
            } else if (e.type == EventType::Rested) {
              c.resting.push_back(e.order_id);
            } else if (e.type == EventType::Fill) {
              ++c.fills;
            }
          }
        }
        off += len;
      }
      c.in.erase(c.in.begin(), c.in.begin() + static_cast<std::ptrdiff_t>(off));
    }
  }
  std::uint64_t acks = 0, fills = 0, rejects = 0, outstanding = 0;
  for (auto& c : conns) {
    acks += c.acks;
    fills += c.fills;
    rejects += c.rejects;
    for (auto& [tag, t] : c.pending)
      if (t >= t_measure && t < t_end) ++outstanding;
    ::close(c.fd);
  }
  auto q = [&](double p) { return static_cast<double>(hist.quantile(p)) * ns_tick / 1e3; };
  std::printf("{\"connections\": %d, \"target_rate\": %.0f, \"duration_s\": %.1f, \"sent\": %" PRIu64
              ", \"new_orders_measured_sent\": %" PRIu64 ", \"acked_measured\": %" PRIu64
              ", \"unacked_at_end\": %" PRIu64 ", \"acks\": %" PRIu64 ", \"rejects\": %" PRIu64 ", \"fills\": %" PRIu64
              ", \"lost_connections\": %" PRIu64 ", \"achieved_rate\": %.0f,\n \"order_to_ack_us\": {\"p50\": %.2f, \"p90\": %.2f, \"p99\": %.2f, \"p99.9\": %.2f, \"p99.99\": %.2f, \"max\": %.2f, \"mean\": %.2f}}\n",
              nconn, rate, duration, sent, sent_measured, measured, outstanding, acks, rejects, fills, lost_conn,
              static_cast<double>(sent_measured) / duration, q(0.5), q(0.9), q(0.99), q(0.999), q(0.9999),
              static_cast<double>(hist.max()) * ns_tick / 1e3, hist.mean() * ns_tick / 1e3);
  if (hist_out)
    if (FILE* f = std::fopen(hist_out, "w")) {
      std::fprintf(f, "# unit: timer ticks; ns_per_tick=%.6f\n", ns_tick);
      std::fputs(hist.to_csv().c_str(), f);
      std::fclose(f);
    }
  return 0;
}
