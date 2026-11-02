#include "matchbox/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define MATCHBOX_KQUEUE 1
#else
#include <sys/epoll.h>
#endif

#include <cerrno>
#include <chrono>
#include <unordered_map>

#include "matchbox/engine.hpp"
#include "matchbox/journal.hpp"
#include "matchbox/spsc_ring.hpp"

namespace matchbox {

// ---------------------------------------------------------------------------------------------
// Protocol

namespace {
template <class T>
void put(std::vector<unsigned char>& d, T v) {
  const auto* p = reinterpret_cast<const unsigned char*>(&v);
  d.insert(d.end(), p, p + sizeof v);
}
template <class T>
T get(const unsigned char* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}
void header(std::vector<unsigned char>& d, MsgType t, std::size_t payload) {
  put<std::uint16_t>(d, static_cast<std::uint16_t>(kHeader + payload));
  d.push_back(static_cast<unsigned char>(t));
}
}  // namespace

void put_logon(std::vector<unsigned char>& d, Account a) {
  header(d, MsgType::Logon, kLogonPayload);
  put(d, a);
}
void put_logon_ack(std::vector<unsigned char>& d, Account a) {
  header(d, MsgType::LogonAck, kLogonPayload);
  put(d, a);
}
void put_new_order(std::vector<unsigned char>& d, const Input& in) {
  header(d, MsgType::NewOrder, kNewOrderPayload);
  put(d, in.client_tag);
  d.push_back(static_cast<unsigned char>(in.side));
  d.push_back(static_cast<unsigned char>(in.ord_type));
  d.push_back(static_cast<unsigned char>(in.tif));
  d.push_back(static_cast<unsigned char>(in.stp));
  d.push_back(in.post_only);
  put(d, in.price);
  put(d, in.stop_price);
  put(d, in.qty);
  put(d, in.display_qty);
}
void put_cancel(std::vector<unsigned char>& d, std::uint64_t tag, OrderId id) {
  header(d, MsgType::Cancel, kCancelPayload);
  put(d, tag);
  put(d, id);
}
void put_replace(std::vector<unsigned char>& d, std::uint64_t tag, OrderId id, Price px, Qty q) {
  header(d, MsgType::Replace, kReplacePayload);
  put(d, tag);
  put(d, id);
  put(d, px);
  put(d, q);
}
void put_event(std::vector<unsigned char>& d, const Event& e) {
  header(d, MsgType::EventMsg, kEventPayload);
  const auto* p = reinterpret_cast<const unsigned char*>(&e);
  d.insert(d.end(), p, p + sizeof e);
}

std::size_t frame_length(const unsigned char* p, std::size_t n) {
  if (n < 2) return 0;
  const std::size_t len = get<std::uint16_t>(p);
  if (len < kHeader || len > kMaxFrame) return SIZE_MAX;
  return n >= len ? len : 0;
}

DecodeStatus decode_frame(const unsigned char* b, std::size_t len, Decoded& o) {
  if (len < kHeader) return DecodeStatus::BadLength;
  const auto t = static_cast<MsgType>(b[2]);
  const unsigned char* p = b + kHeader;
  const std::size_t n = len - kHeader;
  o = Decoded{};
  o.type = t;
  switch (t) {
    case MsgType::Logon:
    case MsgType::LogonAck:
      if (n != kLogonPayload) return DecodeStatus::BadLength;
      o.account = get<Account>(p);
      return DecodeStatus::Ok;
    case MsgType::NewOrder:
      if (n != kNewOrderPayload) return DecodeStatus::BadLength;
      o.input.type = InputType::NewOrder;
      o.input.client_tag = get<std::uint64_t>(p);
      // Enum fields are copied raw; the engine's validation rejects out-of-range values.
      std::memcpy(&o.input.side, p + 8, 1);
      std::memcpy(&o.input.ord_type, p + 9, 1);
      std::memcpy(&o.input.tif, p + 10, 1);
      std::memcpy(&o.input.stp, p + 11, 1);
      o.input.post_only = p[12] ? 1 : 0;
      o.input.price = get<Price>(p + 13);
      o.input.stop_price = get<Price>(p + 21);
      o.input.qty = get<Qty>(p + 29);
      o.input.display_qty = get<Qty>(p + 37);
      return DecodeStatus::Ok;
    case MsgType::Cancel:
      if (n != kCancelPayload) return DecodeStatus::BadLength;
      o.input.type = InputType::Cancel;
      o.input.client_tag = get<std::uint64_t>(p);
      o.input.order_id = get<OrderId>(p + 8);
      return DecodeStatus::Ok;
    case MsgType::Replace:
      if (n != kReplacePayload) return DecodeStatus::BadLength;
      o.input.type = InputType::Replace;
      o.input.client_tag = get<std::uint64_t>(p);
      o.input.order_id = get<OrderId>(p + 8);
      o.input.price = get<Price>(p + 16);
      o.input.qty = get<Qty>(p + 24);
      return DecodeStatus::Ok;
    case MsgType::EventMsg:
      if (n != kEventPayload) return DecodeStatus::BadLength;
      std::memcpy(&o.event, p, sizeof(Event));
      return DecodeStatus::Ok;
  }
  return DecodeStatus::BadType;
}

// ---------------------------------------------------------------------------------------------
// Sockets and poller

void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
void set_nodelay(int fd) {
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

int listen_tcp(std::uint16_t port, bool loopback_only) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 1024) != 0) {
    ::close(fd);
    return -1;
  }
  set_nonblocking(fd);
  return fd;
}

int connect_tcp(const std::string& host, std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  set_nodelay(fd);
  set_nonblocking(fd);
  return fd;
}

#if defined(MATCHBOX_KQUEUE)
Poller::Poller() : pfd_(::kqueue()) {}
Poller::~Poller() {
  if (pfd_ >= 0) ::close(pfd_);
}
bool Poller::add(int fd) {
  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
  return ::kevent(pfd_, &ev, 1, nullptr, 0, nullptr) == 0;
}
bool Poller::set_write(int fd, bool on) {
  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_WRITE, on ? EV_ADD | EV_ENABLE : EV_DELETE, 0, 0, nullptr);
  return ::kevent(pfd_, &ev, 1, nullptr, 0, nullptr) == 0 || !on;
}
void Poller::remove(int fd) {
  struct kevent ev[2];
  EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  ::kevent(pfd_, ev, 2, nullptr, 0, nullptr);
}
int Poller::wait(std::vector<Ready>& out, int timeout_ms) {
  struct kevent evs[256];
  timespec ts{timeout_ms / 1000, static_cast<long>(timeout_ms % 1000) * 1'000'000};
  const int n = ::kevent(pfd_, nullptr, 0, evs, 256, &ts);
  out.clear();
  for (int i = 0; i < n; ++i) {
    const int fd = static_cast<int>(evs[i].ident);
    out.push_back({fd, evs[i].filter == EVFILT_READ, evs[i].filter == EVFILT_WRITE,
                   (evs[i].flags & EV_EOF) != 0 && evs[i].filter == EVFILT_READ && evs[i].data == 0});
  }
  return n < 0 ? 0 : n;
}
#else
Poller::Poller() : pfd_(::epoll_create1(0)) {}
Poller::~Poller() {
  if (pfd_ >= 0) ::close(pfd_);
}
bool Poller::add(int fd) {
  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLRDHUP;
  ev.data.fd = fd;
  return ::epoll_ctl(pfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}
bool Poller::set_write(int fd, bool on) {
  epoll_event ev{};
  ev.events = EPOLLIN | EPOLLRDHUP | (on ? EPOLLOUT : 0u);
  ev.data.fd = fd;
  return ::epoll_ctl(pfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}
void Poller::remove(int fd) { ::epoll_ctl(pfd_, EPOLL_CTL_DEL, fd, nullptr); }
int Poller::wait(std::vector<Ready>& out, int timeout_ms) {
  epoll_event evs[256];
  const int n = ::epoll_wait(pfd_, evs, 256, timeout_ms);
  out.clear();
  for (int i = 0; i < n; ++i)
    out.push_back({evs[i].data.fd, (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0,
                   (evs[i].events & EPOLLOUT) != 0, false});
  return n < 0 ? 0 : n;
}
#endif

// ---------------------------------------------------------------------------------------------
// Server

namespace {
struct Request {
  Input in;
};

inline void cpu_relax() {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

struct CoreSink {
  SpscRing<Event>* out;
  std::atomic<bool>* running;
  std::uint64_t hash = kFnvOffset;
  std::uint64_t n = 0;
  void on_event(const Event& e) {
    hash = fnv1a(hash, &e, sizeof e);
    ++n;
    // Back-pressure: the gateway drains this ring even while it waits to push requests, so
    // spinning here cannot deadlock.
    while (!out->try_push(e)) {
      if (!running->load(std::memory_order_acquire)) return;  // shutting down: gateway gone
      cpu_relax();
    }
  }
};

struct Session {
  int fd;
  Account account = 0;
  std::vector<unsigned char> in;
  std::size_t in_off = 0;
  std::vector<unsigned char> out;
  std::size_t out_off = 0;
  bool want_write = false;
};
}  // namespace

struct Server::Impl {
  ServerConfig cfg;
  int lfd = -1;
  std::atomic<bool> running{false};
  SpscRing<Request> to_core;
  SpscRing<Event> to_gw;
  std::thread core, gw;
  ServerStats st;
  std::atomic<std::uint64_t> sessions{0}, proto_errors{0};

  explicit Impl(const ServerConfig& c) : cfg(c), to_core(c.ring), to_gw(c.ring) {}

  void core_loop() {
    CoreSink sink{&to_gw, &running};
    auto engine = std::make_unique<Engine<CoreSink>>(sink, EngineConfig{cfg.order_capacity, 1u << 16, 1u << 12, 0});
    JournalWriter jw;
    const bool journal = !cfg.journal_path.empty() && jw.open(cfg.journal_path, {1 << 20, cfg.fsync_every});
    std::uint64_t seq = 0;
    Request r;
    std::uint32_t idle = 0;
    for (;;) {
      if (to_core.try_pop(r)) {
        idle = 0;
        r.in.seq = ++seq;
        if (journal) jw.append(r.in);  // write-ahead: journaled before it is processed
        engine->process(r.in);
        continue;
      }
      if (journal && idle == 0) jw.flush();  // group: flush when the burst ends
      if (!running.load(std::memory_order_acquire)) break;
      if (++idle > 1024 && !cfg.spin) std::this_thread::sleep_for(std::chrono::microseconds(50));
      else cpu_relax();
    }
    if (journal) {
      jw.sync();
      st.journal_records = jw.records();
      st.journal_fsyncs = jw.fsyncs();
    }
    st.inputs = seq;
    st.events = sink.n;
    st.event_hash = sink.hash;
  }

  void gw_loop() {
    Poller poller;
    poller.add(lfd);
    std::unordered_map<int, Session> sess;
    std::unordered_map<Account, int> by_account;
    std::vector<Poller::Ready> ready;
    Event ev;
    auto close_session = [&](int fd) {
      auto it = sess.find(fd);
      if (it == sess.end()) return;
      if (it->second.account) {
        auto a = by_account.find(it->second.account);
        if (a != by_account.end() && a->second == fd) by_account.erase(a);
      }
      poller.remove(fd);
      ::close(fd);
      sess.erase(it);
    };
    auto flush = [&](Session& s) {
      while (s.out_off < s.out.size()) {
        const ssize_t w = ::send(s.fd, s.out.data() + s.out_off, s.out.size() - s.out_off, 0);
        if (w > 0) {
          s.out_off += static_cast<std::size_t>(w);
          continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          if (!s.want_write) {
            poller.set_write(s.fd, true);
            s.want_write = true;
          }
          return true;
        }
        return false;
      }
      s.out.clear();
      s.out_off = 0;
      if (s.want_write) {
        poller.set_write(s.fd, false);
        s.want_write = false;
      }
      return true;
    };
    std::vector<int> dirty;
    auto drain_events = [&]() {
      while (to_gw.try_pop(ev)) {
        auto a = by_account.find(ev.account);
        if (a == by_account.end()) continue;  // account not connected: event dropped (journaled anyway)
        Session& s = sess[a->second];
        if (s.out.empty()) dirty.push_back(s.fd);
        put_event(s.out, ev);
      }
    };
    auto flush_dirty = [&]() {
      for (int fd : dirty) {
        auto it = sess.find(fd);
        if (it != sess.end() && !flush(it->second)) close_session(fd);
      }
      dirty.clear();
    };
    while (running.load(std::memory_order_acquire)) {
      poller.wait(ready, cfg.spin ? 0 : 1);
      for (const auto& r : ready) {
        if (r.fd == lfd) {
          for (;;) {
            const int cfd = ::accept(lfd, nullptr, nullptr);
            if (cfd < 0) break;
            set_nonblocking(cfd);
            set_nodelay(cfd);
            poller.add(cfd);
            Session ns{};
            ns.fd = cfd;
            sess.emplace(cfd, std::move(ns));
            sessions.fetch_add(1, std::memory_order_relaxed);
          }
          continue;
        }
        auto it = sess.find(r.fd);
        if (it == sess.end()) continue;
        Session& s = it->second;
        if (r.writable && !flush(s)) {
          close_session(r.fd);
          continue;
        }
        if (!r.readable) continue;
        bool alive = true;
        unsigned char buf[64 * 1024];
        for (;;) {
          const ssize_t n = ::recv(s.fd, buf, sizeof buf, 0);
          if (n > 0) {
            s.in.insert(s.in.end(), buf, buf + n);
            continue;
          }
          if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) alive = false;
          break;
        }
        // Parse complete frames.
        while (alive) {
          const std::size_t len = frame_length(s.in.data() + s.in_off, s.in.size() - s.in_off);
          if (len == 0) break;
          Decoded d;
          if (len == SIZE_MAX || decode_frame(s.in.data() + s.in_off, len, d) != DecodeStatus::Ok) {
            proto_errors.fetch_add(1, std::memory_order_relaxed);
            alive = false;
            break;
          }
          s.in_off += len;
          if (d.type == MsgType::Logon) {
            if (s.account == 0 && d.account != 0) {
              s.account = d.account;
              by_account[d.account] = s.fd;
              if (s.out.empty()) dirty.push_back(s.fd);
              put_logon_ack(s.out, d.account);
            }
            continue;
          }
          if (s.account == 0 || (d.type != MsgType::NewOrder && d.type != MsgType::Cancel && d.type != MsgType::Replace)) {
            proto_errors.fetch_add(1, std::memory_order_relaxed);
            alive = false;
            break;
          }
          Request rq{d.input};
          rq.in.account = s.account;
          while (!to_core.try_push(rq)) {  // core is busy: keep draining its output meanwhile
            drain_events();
            cpu_relax();
          }
        }
        if (s.in_off == s.in.size()) {
          s.in.clear();
          s.in_off = 0;
        }
        if (!alive) close_session(r.fd);
      }
      drain_events();
      flush_dirty();
    }
    // Shutdown: let the core finish, then deliver what is left.
    for (auto& [fd, s] : sess) ::close(fd);
    sess.clear();
  }
};

Server::Server(const ServerConfig& c) : impl_(std::make_unique<Impl>(c)) {}
Server::~Server() { stop(); }

bool Server::start() {
  impl_->lfd = listen_tcp(impl_->cfg.port, impl_->cfg.loopback_only);
  if (impl_->lfd < 0) return false;
  sockaddr_in a{};
  socklen_t al = sizeof a;
  ::getsockname(impl_->lfd, reinterpret_cast<sockaddr*>(&a), &al);
  port_ = ntohs(a.sin_port);
  impl_->running.store(true, std::memory_order_release);
  impl_->core = std::thread([this] { impl_->core_loop(); });
  impl_->gw = std::thread([this] { impl_->gw_loop(); });
  return true;
}

void Server::stop() {
  if (!impl_ || !impl_->running.exchange(false)) return;
  impl_->gw.join();
  impl_->core.join();
  ::close(impl_->lfd);
}

ServerStats Server::stats() const {
  ServerStats s = impl_->st;
  s.sessions = impl_->sessions.load();
  s.protocol_errors = impl_->proto_errors.load();
  return s;
}

}  // namespace matchbox
