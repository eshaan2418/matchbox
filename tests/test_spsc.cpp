#include <gtest/gtest.h>

#include <thread>

#include "matchbox/spsc_ring.hpp"
#include "matchbox/types.hpp"

using namespace matchbox;

TEST(Spsc, TwoThreadStressWithEvents) {
  constexpr std::uint64_t kN = 1'000'000;
  SpscRing<Event> r(1024);
  std::thread p([&] {
    for (std::uint64_t i = 0; i < kN; ++i) {
      Event e{};
      e.seq = i;
      e.qty = static_cast<Qty>(i * 3);
      while (!r.try_push(e)) std::this_thread::yield();
    }
  });
  Event e{};
  for (std::uint64_t i = 0; i < kN;) {
    if (r.try_pop(e)) {
      ASSERT_EQ(e.seq, i);
      ASSERT_EQ(e.qty, static_cast<Qty>(i * 3));
      ++i;
    }
  }
  p.join();
}
