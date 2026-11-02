// Binary order-entry protocol, a kqueue/epoll poller, and the exchange server.
//
// Wire format (little-endian hosts only: x86-64, arm64): every frame is
//   u16 total length (including this 3-byte header) | u8 type | payload
//
// Threads: one gateway thread (sockets, framing, routing) and one core thread (sequencer +
// journal + matching engine), connected by two SPSC rings. The core is the only writer of
// sequence numbers, so the journal order is the processing order and replay is exact.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "matchbox/types.hpp"

namespace matchbox {

enum class MsgType : std::uint8_t {
  Logon = 1,
  NewOrder = 2,
  Cancel = 3,
  Replace = 4,
  LogonAck = 101,
  EventMsg = 102,
};

inline constexpr std::size_t kHeader = 3;
inline constexpr std::size_t kNewOrderPayload = 8 + 5 + 4 * 8;  // 45
inline constexpr std::size_t kCancelPayload = 16;
inline constexpr std::size_t kReplacePayload = 32;
inline constexpr std::size_t kLogonPayload = 4;
inline constexpr std::size_t kEventPayload = sizeof(Event);
inline constexpr std::size_t kMaxFrame = 256;

// ---- encoders (append a frame to dst)
void put_logon(std::vector<unsigned char>& dst, Account a);
void put_new_order(std::vector<unsigned char>& dst, const Input& in);  // uses tag/side/types/prices
void put_cancel(std::vector<unsigned char>& dst, std::uint64_t tag, OrderId id);
void put_replace(std::vector<unsigned char>& dst, std::uint64_t tag, OrderId id, Price px, Qty q);
void put_event(std::vector<unsigned char>& dst, const Event& e);
void put_logon_ack(std::vector<unsigned char>& dst, Account a);

// ---- decoder: one frame at buf (len bytes, already length-checked by the framer).
enum class DecodeStatus : std::uint8_t { Ok, BadType, BadLength };
struct Decoded {
  MsgType type;
  Account account;  // Logon / LogonAck
  Input input;      // NewOrder / Cancel / Replace (seq/account filled by the server)
  Event event;      // EventMsg
};
DecodeStatus decode_frame(const unsigned char* buf, std::size_t len, Decoded& out);

// Framer: returns the length of the first complete frame in [p, p+n), 0 if incomplete, or
// SIZE_MAX if the header is invalid (length < header or > kMaxFrame).
std::size_t frame_length(const unsigned char* p, std::size_t n);

// ---- poller abstraction (kqueue on macOS/BSD, epoll on Linux)
class Poller {
 public:
  struct Ready {
    int fd;
    bool readable;
    bool writable;
    bool hangup;
  };
  Poller();
  ~Poller();
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;
  bool add(int fd);                     // read interest
  bool set_write(int fd, bool on);      // toggle write interest
  void remove(int fd);
  int wait(std::vector<Ready>& out, int timeout_ms);  // returns number of ready fds

 private:
  int pfd_ = -1;
};

int listen_tcp(std::uint16_t port, bool loopback_only);  // returns fd (non-blocking) or -1
int connect_tcp(const std::string& host, std::uint16_t port);  // blocking connect, then non-blocking
void set_nonblocking(int fd);
void set_nodelay(int fd);

// ---- server
struct ServerConfig {
  std::uint16_t port = 9000;
  bool loopback_only = true;
  std::string journal_path;  // empty = no journal
  std::uint32_t fsync_every = 0;
  bool spin = true;          // busy-poll gateway and core (lowest latency, burns 2 cores)
  std::size_t ring = 1 << 16;
  std::size_t order_capacity = 1 << 20;
};

struct ServerStats {
  std::uint64_t inputs = 0;
  std::uint64_t events = 0;
  std::uint64_t event_hash = kFnvOffset;
  std::uint64_t journal_records = 0;
  std::uint64_t journal_fsyncs = 0;
  std::uint64_t sessions = 0;
  std::uint64_t protocol_errors = 0;
};

class Server {
 public:
  explicit Server(const ServerConfig& c);
  ~Server();
  bool start();   // binds and starts threads
  void stop();    // stops threads, closes sockets, flushes journal
  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] ServerStats stats() const;  // valid after stop()

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_ = 0;
};

}  // namespace matchbox
