// Uniform test harness over both engines.
#pragma once

#include <memory>
#include <vector>

#include "matchbox/engine.hpp"
#include "matchbox/reference_engine.hpp"

namespace matchbox::test {

struct VecSink {
  std::vector<Event> ev;
  void on_event(const Event& e) { ev.push_back(e); }
};

struct Fast {
  VecSink sink;
  std::unique_ptr<Engine<VecSink>> e = std::make_unique<Engine<VecSink>>(sink, EngineConfig{64, 16, 4, 0});
  std::vector<Event> run(const Input& in) {
    sink.ev.clear();
    e->process(in);
    return sink.ev;
  }
  auto levels(Side s) const {
    std::vector<std::tuple<Price, Qty, Qty, std::size_t>> v;
    for (auto& l : e->levels(s)) v.emplace_back(l.price, l.leaves, l.visible, l.orders);
    return v;
  }
  auto queue(Side s, Price px) const {
    std::vector<std::tuple<OrderId, Qty, Qty>> v;
    e->for_each_in_queue(s, px, [&](OrderId id, Qty l, Qty vis) { v.emplace_back(id, l, vis); });
    return v;
  }
  std::size_t resting() const { return e->resting(); }
  std::size_t stops() const { return e->stops(); }
};

struct Ref {
  ReferenceEngine e;
  std::vector<Event> run(const Input& in) {
    std::vector<Event> out;
    e.process(in, out);
    return out;
  }
  auto levels(Side s) const {
    std::vector<std::tuple<Price, Qty, Qty, std::size_t>> v;
    for (auto& l : e.levels(s)) v.emplace_back(l.price, l.leaves, l.visible, l.orders);
    return v;
  }
  auto queue(Side s, Price px) const { return e.queue(s, px); }
  std::size_t resting() const { return e.resting(); }
  std::size_t stops() const { return e.stops(); }
};

inline int count(const std::vector<Event>& ev, EventType t) {
  int n = 0;
  for (auto& e : ev) n += e.type == t;
  return n;
}

}  // namespace matchbox::test
