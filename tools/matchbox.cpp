// matchbox CLI: serve (exchange server) | replay (journal -> engine, event hash + throughput)
#include <signal.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "matchbox/engine.hpp"
#include "matchbox/journal.hpp"
#include "matchbox/net.hpp"

using namespace matchbox;

namespace {
volatile sig_atomic_t g_stop = 0;
void on_sig(int) { g_stop = 1; }

const char* arg(int argc, char** argv, const char* key, const char* def) {
  for (int i = 2; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}

struct HashSink {
  std::uint64_t h = kFnvOffset, n = 0;
  void on_event(const Event& e) {
    h = fnv1a(h, &e, sizeof e);
    ++n;
  }
};

int serve(int argc, char** argv) {
  ServerConfig c;
  c.port = static_cast<std::uint16_t>(std::atoi(arg(argc, argv, "--port", "9000")));
  c.journal_path = arg(argc, argv, "--journal", "");
  c.fsync_every = static_cast<std::uint32_t>(std::atoi(arg(argc, argv, "--fsync-every", "0")));
  c.spin = std::atoi(arg(argc, argv, "--spin", "1")) != 0;
  c.loopback_only = std::atoi(arg(argc, argv, "--loopback", "1")) != 0;
  const double duration = std::atof(arg(argc, argv, "--duration", "0"));
  Server s(c);
  if (!s.start()) {
    std::fprintf(stderr, "cannot listen on port %u\n", c.port);
    return 1;
  }
  std::fprintf(stderr, "matchbox listening on %u (journal=%s, fsync_every=%u, spin=%d)\n", s.port(),
               c.journal_path.empty() ? "off" : c.journal_path.c_str(), c.fsync_every, c.spin ? 1 : 0);
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
  const auto t0 = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (duration > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= duration) break;
  }
  s.stop();
  const ServerStats st = s.stats();
  std::printf("{\"inputs\": %" PRIu64 ", \"events\": %" PRIu64 ", \"event_hash\": \"%016" PRIx64
              "\", \"journal_records\": %" PRIu64 ", \"journal_fsyncs\": %" PRIu64
              ", \"sessions\": %" PRIu64 ", \"protocol_errors\": %" PRIu64 "}\n",
              st.inputs, st.events, st.event_hash, st.journal_records, st.journal_fsyncs, st.sessions,
              st.protocol_errors);
  return 0;
}

int replay(int argc, char** argv) {
  if (argc < 3) return 1;
  std::vector<Input> inputs;
  JournalReadResult r;
  if (!read_journal_raw(argv[2], inputs, r)) {
    std::fprintf(stderr, "cannot read %s\n", argv[2]);
    return 1;
  }
  HashSink sink;
  auto e = std::make_unique<Engine<HashSink>>(sink, EngineConfig{1u << 22, 1u << 16, 1u << 12, 0});
  const auto t0 = std::chrono::steady_clock::now();
  for (const Input& in : inputs) e->process(in);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("{\"records\": %" PRIu64 ", \"truncated_tail\": %s, \"corrupt\": %s, \"events\": %" PRIu64
              ", \"event_hash\": \"%016" PRIx64 "\", \"seconds\": %.4f, \"inputs_per_sec\": %.0f}\n",
              r.records, r.truncated_tail ? "true" : "false", r.corrupt ? "true" : "false", sink.n, sink.h, s,
              static_cast<double>(inputs.size()) / s);
  return r.corrupt ? 2 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: matchbox serve [--port P] [--journal F] [--fsync-every N] [--spin 0|1] [--duration S]\n"
                         "       matchbox replay JOURNAL\n");
    return 1;
  }
  if (std::strcmp(argv[1], "serve") == 0) return serve(argc, argv);
  if (std::strcmp(argv[1], "replay") == 0) return replay(argc, argv);
  std::fprintf(stderr, "unknown command %s\n", argv[1]);
  return 1;
}
