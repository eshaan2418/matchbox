// In-process engine benchmark: pre-generated realistic order flow -> engine.
//   --impl fast|ref     optimised engine or the std::map reference
//   --latency           time every input individually (CNTVCT/steady_clock), else throughput only
//   --journal PATH      journal every input before processing (write-ahead), --fsync-every N
#include <pthread.h>
#if defined(__APPLE__)
#include <sys/qos.h>
#endif

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "matchbox/alloc_counter.hpp"
#include "matchbox/engine.hpp"
#include "matchbox/gen.hpp"
#include "matchbox/journal.hpp"
#include "matchbox/latency.hpp"
#include "matchbox/reference_engine.hpp"

using namespace matchbox;

namespace {
const char* arg(int argc, char** argv, const char* key, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}
bool flag(int argc, char** argv, const char* key) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return true;
  return false;
}
// Counts events; hashes them only with --hash (an FNV-1a over 72 bytes per event costs about as
// much as matching itself, so it is off for timing runs and used for determinism checks).
struct CountSink {
  bool hash = false;
  std::uint64_t n = 0, fills = 0, h = kFnvOffset;
  void on_event(const Event& e) {
    ++n;
    fills += e.type == EventType::Fill;
    if (hash) h = fnv1a(h, &e, sizeof e);
  }
};
}  // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  const std::string impl = arg(argc, argv, "--impl", "fast");
  const std::size_t n = std::strtoull(arg(argc, argv, "--inputs", "5000000"), nullptr, 10);
  const bool latency = flag(argc, argv, "--latency");
  const char* jpath = arg(argc, argv, "--journal", nullptr);
  const auto fsync_every = static_cast<std::uint32_t>(std::atoi(arg(argc, argv, "--fsync-every", "0")));
  const char* hist_out = arg(argc, argv, "--hist-out", nullptr);
  GenConfig gc{.seed = std::strtoull(arg(argc, argv, "--seed", "42"), nullptr, 10)};
  gc.realistic = true;
  Gen g(gc);
  std::vector<Input> inputs(n);
  for (auto& in : inputs) in = g.next();

  CountSink sink;
  sink.hash = flag(argc, argv, "--hash");
  auto fast = std::make_unique<Engine<CountSink>>(sink, EngineConfig{1u << 20, 1u << 14, 1u << 12, 0});
  ReferenceEngine ref;
  std::vector<Event> scratch;
  scratch.reserve(64);
  JournalWriter jw;
  if (jpath && !jw.open(jpath, {1 << 20, fsync_every})) {
    std::fprintf(stderr, "cannot open journal\n");
    return 1;
  }
  Histogram hist;
  const double ns_tick = Clock::ns_per_tick();
  const std::size_t warm = n / 10;  // first 10% excluded from latency and allocation counts
  std::uint64_t allocs_before = 0;
  auto step = [&](const Input& in) {
    if (jpath) jw.append(in);
    if (impl == "ref") {
      scratch.clear();
      ref.process(in, scratch);
      for (auto& e : scratch) sink.on_event(e);
    } else {
      fast->process(in);
    }
  };
  const auto w0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < n; ++i) {
    if (i == warm) allocs_before = alloc_count();
    if (latency && i >= warm) {
      const std::uint64_t t0 = Clock::now();
      step(inputs[i]);
      hist.record(Clock::now() - t0);
    } else {
      step(inputs[i]);
    }
  }
  if (jpath) jw.sync();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
  const std::uint64_t allocs = alloc_count() - allocs_before;
  auto q = [&](double p) { return static_cast<double>(hist.quantile(p)) * ns_tick; };
  std::printf("{\"impl\": \"%s\", \"inputs\": %zu, \"events\": %" PRIu64 ", \"fills\": %" PRIu64
              ", \"event_hash\": \"%016" PRIx64 "\", \"seconds\": %.4f, \"inputs_per_sec\": %.0f, \"ns_per_input\": %.2f"
              ", \"journal\": %s, \"fsync_every\": %u, \"fsyncs\": %" PRIu64 ", \"allocs_after_warmup\": %" PRIu64,
              impl.c_str(), n, sink.n, sink.fills, sink.h, secs, static_cast<double>(n) / secs, secs * 1e9 / static_cast<double>(n),
              jpath ? "true" : "false", fsync_every, jw.fsyncs(), allocs);
  if (latency)
    std::printf(", \"timer_ns_per_tick\": %.3f, \"latency_ns\": {\"p50\": %.0f, \"p90\": %.0f, \"p99\": %.0f, \"p99.9\": %.0f, \"p99.99\": %.0f, \"max\": %.0f, \"mean\": %.1f}",
                ns_tick, q(0.5), q(0.9), q(0.99), q(0.999), q(0.9999), static_cast<double>(hist.max()) * ns_tick, hist.mean() * ns_tick);
  std::printf("}\n");
  if (hist_out)
    if (FILE* f = std::fopen(hist_out, "w")) {
      std::fprintf(f, "# unit: timer ticks; ns_per_tick=%.6f\n", ns_tick);
      std::fputs(hist.to_csv().c_str(), f);
      std::fclose(f);
    }
  return 0;
}
