// The engine's hot path must not allocate once pools/levels/stop books are at working capacity.
#include <gtest/gtest.h>

#include "matchbox/alloc_counter.hpp"
#include "matchbox/engine.hpp"
#include "matchbox/gen.hpp"

using namespace matchbox;

namespace {
struct CountSink {
  std::uint64_t n = 0;
  void on_event(const Event&) { ++n; }
};
}  // namespace

TEST(Alloc, NoHeapAllocationAfterWarmup) {
  if (!alloc_hook_enabled()) GTEST_SKIP() << "allocation hook disabled under ThreadSanitizer";
  GenConfig cfg{.seed = 9};
  cfg.realistic = true;
  Gen g(cfg);
  std::vector<Input> inputs(2'000'000);
  for (auto& in : inputs) in = g.next();
  CountSink sink;
  auto e = std::make_unique<Engine<CountSink>>(sink, EngineConfig{1u << 18, 1u << 14, 1u << 12, 0});
  for (std::size_t i = 0; i < inputs.size() / 2; ++i) e->process(inputs[i]);
  const auto before = alloc_count();
  for (std::size_t i = inputs.size() / 2; i < inputs.size(); ++i) e->process(inputs[i]);
  EXPECT_EQ(alloc_count() - before, 0u);
  EXPECT_GT(sink.n, 1'000'000u);
}
