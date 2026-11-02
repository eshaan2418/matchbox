#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "harness.hpp"
#include "matchbox/gen.hpp"
#include "matchbox/journal.hpp"

using namespace matchbox;

namespace {
std::string tmp(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }
}  // namespace

TEST(Crc32c, KnownVector) {
  // RFC 3720 test vector: CRC32C("123456789") = 0xE3069283.
  EXPECT_EQ(crc32c("123456789", 9), 0xE3069283u);
}

TEST(Journal, RoundTripAndReplayDeterminism) {
  const auto path = tmp("matchbox_journal_test.bin");
  Gen g({.seed = 11});
  std::vector<Input> inputs(50'000);
  for (auto& in : inputs) in = g.next();
  {
    JournalWriter w;
    ASSERT_TRUE(w.open(path, {4096, 1000}));
    for (auto& in : inputs) ASSERT_TRUE(w.append(in));
    ASSERT_TRUE(w.sync());
    EXPECT_GE(w.fsyncs(), 50u);
  }
  std::vector<Input> back;
  auto r = read_journal(path, [&](const Input& in) { back.push_back(in); });
  ASSERT_EQ(r.records, inputs.size());
  EXPECT_FALSE(r.truncated_tail);
  EXPECT_FALSE(r.corrupt);
  ASSERT_EQ(std::memcmp(back.data(), inputs.data(), inputs.size() * sizeof(Input)), 0);
  // Replaying the journal produces the same event stream as the original run.
  auto hash = [](const std::vector<Input>& v) {
    test::Fast f;
    std::uint64_t h = kFnvOffset;
    for (auto& in : v)
      for (auto& e : f.run(in)) h = fnv1a(h, &e, sizeof e);
    return h;
  };
  EXPECT_EQ(hash(inputs), hash(back));
  std::filesystem::remove(path);
}

TEST(Journal, TornTailAndCorruptionStopReplay) {
  const auto path = tmp("matchbox_journal_torn.bin");
  Gen g({.seed = 12});
  {
    JournalWriter w;
    ASSERT_TRUE(w.open(path, {}));
    for (int i = 0; i < 100; ++i) w.append(g.next());
  }
  const auto full = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, full - 10);  // crash mid-record
  auto r = read_journal(path, [](const Input&) {});
  EXPECT_EQ(r.records, 99u);
  EXPECT_TRUE(r.truncated_tail);
  {  // flip a byte inside record 50's payload
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(50 * 80 + 8 + 20);
    f.put('\x7f');
  }
  r = read_journal(path, [](const Input&) {});
  EXPECT_EQ(r.records, 50u);
  EXPECT_TRUE(r.corrupt);
  EXPECT_EQ(r.corrupt_offset, 50u * 80u);
  std::filesystem::remove(path);
}
