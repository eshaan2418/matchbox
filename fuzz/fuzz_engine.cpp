// libFuzzer: arbitrary bytes are interpreted as a stream of Inputs (raw 72-byte records, any field
// values) and fed to both engines, which must agree event-for-event and never crash.
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "matchbox/engine.hpp"
#include "matchbox/reference_engine.hpp"

using namespace matchbox;

namespace {
struct VecSink {
  std::vector<Event> ev;
  void on_event(const Event& e) { ev.push_back(e); }
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  VecSink sink;
  auto fast = std::make_unique<Engine<VecSink>>(sink, EngineConfig{16, 4, 2, 0});
  ReferenceEngine ref;
  std::vector<Event> rev;
  std::uint64_t seq = 0;
  for (std::size_t off = 0; off + sizeof(Input) <= size; off += sizeof(Input)) {
    Input in;
    std::memcpy(&in, data + off, sizeof in);
    in.seq = ++seq;  // the sequencer always assigns increasing sequence numbers
    // Keep magnitudes in a range where int64 sums cannot overflow (the gateway would bound
    // these via risk checks in production).
    in.qty %= 1'000'000;
    in.display_qty %= 1'000'000;
    in.price %= 1'000'000;
    in.stop_price %= 1'000'000;
    if (in.order_id > seq + 2) in.order_id %= (seq + 2);
    in.account %= 4;
    sink.ev.clear();
    rev.clear();
    fast->process(in);
    ref.process(in, rev);
    if (sink.ev.size() != rev.size()) std::abort();
    for (std::size_t k = 0; k < rev.size(); ++k)
      if (!(sink.ev[k] == rev[k])) std::abort();
  }
  return 0;
}
