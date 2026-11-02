// End-to-end over loopback TCP: sessions, routing of events to both counterparties, protocol
// errors, and journal replay reproducing the live event hash.
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "harness.hpp"
#include "matchbox/journal.hpp"
#include "matchbox/net.hpp"

using namespace matchbox;
using namespace std::chrono_literals;

namespace {
struct Client {
  int fd = -1;
  std::vector<unsigned char> in;
  std::vector<Event> events;
  bool logged_on = false;
  explicit Client(std::uint16_t port) { fd = connect_tcp("127.0.0.1", port); }
  ~Client() {
    if (fd >= 0) ::close(fd);
  }
  void send(const std::vector<unsigned char>& b) {
    std::size_t off = 0;
    while (off < b.size()) {
      const ssize_t w = ::send(fd, b.data() + off, b.size() - off, 0);
      if (w > 0) off += static_cast<std::size_t>(w);
      else std::this_thread::yield();
    }
  }
  // Reads until pred() or timeout.
  template <class P>
  bool pump(P pred, std::chrono::milliseconds t = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + t;
    unsigned char buf[4096];
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n == 0) return pred();
      if (n > 0) in.insert(in.end(), buf, buf + n);
      std::size_t off = 0;
      for (;;) {
        const std::size_t len = frame_length(in.data() + off, in.size() - off);
        if (len == 0 || len == SIZE_MAX) break;
        Decoded d;
        if (decode_frame(in.data() + off, len, d) == DecodeStatus::Ok) {
          if (d.type == MsgType::LogonAck) logged_on = true;
          if (d.type == MsgType::EventMsg) events.push_back(d.event);
        }
        off += len;
      }
      in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(off));
      if (n < 0) std::this_thread::sleep_for(100us);
    }
    return true;
  }
  bool closed_by_server(std::chrono::milliseconds t = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + t;
    unsigned char buf[256];
    while (std::chrono::steady_clock::now() < deadline) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n == 0) return true;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
};
}  // namespace

TEST(Gateway, EndToEndRoutingAndJournalReplay) {
  const auto jpath = (std::filesystem::temp_directory_path() / "matchbox_gw_journal.bin").string();
  ServerConfig cfg;
  cfg.port = 0;
  cfg.journal_path = jpath;
  cfg.spin = false;
  cfg.ring = 1024;
  cfg.order_capacity = 4096;
  Server srv(cfg);
  ASSERT_TRUE(srv.start());
  Client a(srv.port()), b(srv.port());
  ASSERT_GE(a.fd, 0);
  std::vector<unsigned char> m;
  put_logon(m, 1);
  a.send(m);
  m.clear();
  put_logon(m, 2);
  b.send(m);
  ASSERT_TRUE(a.pump([&] { return a.logged_on; }));
  ASSERT_TRUE(b.pump([&] { return b.logged_on; }));
  // A rests a sell; B lifts it with a buy: both must receive their fill.
  m.clear();
  Input s = make_limit(0, 0, Side::Sell, 100, 5);
  s.client_tag = 11;
  put_new_order(m, s);
  a.send(m);
  ASSERT_TRUE(a.pump([&] { return a.events.size() >= 2; }));  // Accepted, Rested
  m.clear();
  Input buy = make_limit(0, 0, Side::Buy, 101, 3);
  buy.client_tag = 22;
  put_new_order(m, buy);
  b.send(m);
  ASSERT_TRUE(b.pump([&] { return b.events.size() >= 2; }));
  ASSERT_TRUE(a.pump([&] { return a.events.size() >= 3; }));
  EXPECT_EQ(b.events[0].type, EventType::Accepted);
  EXPECT_EQ(b.events[0].client_tag, 22u);
  EXPECT_EQ(b.events[1].type, EventType::Fill);
  EXPECT_EQ(b.events[1].price, 100);
  EXPECT_EQ(a.events[2].type, EventType::Fill);
  EXPECT_EQ(a.events[2].leaves, 2);
  // Unknown cancel and cross-account cancel are rejected to the sender.
  m.clear();
  put_cancel(m, 33, 1);  // order 1 belongs to account 1
  b.send(m);
  ASSERT_TRUE(b.pump([&] { return b.events.size() >= 3; }));
  EXPECT_EQ(b.events[2].reason, Reason::NotOwner);
  // A malformed frame closes the session.
  Client c(srv.port());
  std::vector<unsigned char> junk = {3, 0, 99};
  c.send(junk);
  EXPECT_TRUE(c.closed_by_server());
  std::this_thread::sleep_for(50ms);
  srv.stop();
  const ServerStats st = srv.stats();
  EXPECT_EQ(st.inputs, 3u);
  EXPECT_EQ(st.journal_records, 3u);
  EXPECT_GE(st.protocol_errors, 1u);
  // Replay the journal: identical event stream hash.
  test::Fast f;
  std::uint64_t h = kFnvOffset, n = 0;
  read_journal(jpath, [&](const Input& in) {
    for (auto& e : f.run(in)) {
      h = fnv1a(h, &e, sizeof e);
      ++n;
    }
  });
  EXPECT_EQ(n, st.events);
  EXPECT_EQ(h, st.event_hash);
  std::filesystem::remove(jpath);
}
