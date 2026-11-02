// Append-only input journal. Every sequenced input is written before it is processed, so the
// engine state after a crash is exactly reproducible by replaying the journal.
//
// Record: u32 payload length | u32 CRC32C(payload) | payload (one Input, 72 bytes). A torn or
// corrupt tail (crash mid-write, bit rot) is detected by length/CRC and replay stops there.
// Durability is a policy: buffered (OS page cache), or fsync every N records / T microseconds
// (group commit). The journal's cost is measured in bench/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "matchbox/types.hpp"

namespace matchbox {

std::uint32_t crc32c(const void* data, std::size_t n, std::uint32_t crc = 0);

struct JournalPolicy {
  std::size_t buffer_bytes = 1 << 20;  // user-space buffer; flushed when full or on sync
  std::uint32_t fsync_every = 0;       // 0 = never fsync (rely on page cache); else group size
};

class JournalWriter {
 public:
  JournalWriter() = default;
  ~JournalWriter();
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;

  bool open(const std::string& path, const JournalPolicy& p);  // truncates
  bool append(const Input& in);                                // buffered
  bool flush();                                                // write(2) the buffer
  bool sync();                                                 // flush + fsync
  void close();
  [[nodiscard]] std::uint64_t records() const { return records_; }
  [[nodiscard]] std::uint64_t fsyncs() const { return fsyncs_; }
  [[nodiscard]] bool ok() const { return fd_ >= 0 && !error_; }

 private:
  int fd_ = -1;
  JournalPolicy policy_;
  std::vector<unsigned char> buf_;
  std::size_t used_ = 0;
  std::uint64_t records_ = 0;
  std::uint64_t since_sync_ = 0;
  std::uint64_t fsyncs_ = 0;
  bool error_ = false;
};

struct JournalReadResult {
  std::uint64_t records = 0;
  bool truncated_tail = false;  // partial record at the end (stopped cleanly)
  bool corrupt = false;         // CRC mismatch (stopped)
  std::uint64_t corrupt_offset = 0;
};

// Calls f(const Input&) for every valid record in order.
template <class F>
JournalReadResult read_journal(const std::string& path, F&& f);

// Non-template implementation detail.
bool read_journal_raw(const std::string& path, std::vector<Input>& out, JournalReadResult& r);

template <class F>
JournalReadResult read_journal(const std::string& path, F&& f) {
  std::vector<Input> v;
  JournalReadResult r;
  read_journal_raw(path, v, r);
  for (const Input& in : v) f(in);
  return r;
}

}  // namespace matchbox
