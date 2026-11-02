#include "matchbox/journal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstring>

namespace matchbox {

namespace {
// CRC32C (Castagnoli), table-driven; hardware CRC instructions would be a further optimisation.
constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    t[i] = c;
  }
  return t;
}
constexpr auto kTable = make_table();
}  // namespace

std::uint32_t crc32c(const void* data, std::size_t n, std::uint32_t crc) {
  const auto* p = static_cast<const unsigned char*>(data);
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

JournalWriter::~JournalWriter() { close(); }

bool JournalWriter::open(const std::string& path, const JournalPolicy& p) {
  close();
  policy_ = p;
  buf_.assign(p.buffer_bytes < 4096 ? 4096 : p.buffer_bytes, 0);
  used_ = 0;
  records_ = since_sync_ = fsyncs_ = 0;
  error_ = false;
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  return fd_ >= 0;
}

bool JournalWriter::append(const Input& in) {
  constexpr std::size_t kRec = 8 + sizeof(Input);
  if (used_ + kRec > buf_.size() && !flush()) return false;
  const std::uint32_t len = sizeof(Input);
  const std::uint32_t crc = crc32c(&in, sizeof in);
  std::memcpy(buf_.data() + used_, &len, 4);
  std::memcpy(buf_.data() + used_ + 4, &crc, 4);
  std::memcpy(buf_.data() + used_ + 8, &in, sizeof in);
  used_ += kRec;
  ++records_;
  if (policy_.fsync_every && ++since_sync_ >= policy_.fsync_every) return sync();
  return true;
}

bool JournalWriter::flush() {
  std::size_t off = 0;
  while (off < used_) {
    const ssize_t w = ::write(fd_, buf_.data() + off, used_ - off);
    if (w <= 0) {
      error_ = true;
      return false;
    }
    off += static_cast<std::size_t>(w);
  }
  used_ = 0;
  return true;
}

bool JournalWriter::sync() {
  if (!flush()) return false;
#if defined(__APPLE__)
  // fsync on macOS does not flush the drive cache; F_FULLFSYNC does (much slower). We use
  // fsync for parity with Linux semantics and document the difference.
  if (::fsync(fd_) != 0) error_ = true;
#else
  if (::fdatasync(fd_) != 0) error_ = true;
#endif
  ++fsyncs_;
  since_sync_ = 0;
  return !error_;
}

void JournalWriter::close() {
  if (fd_ >= 0) {
    flush();
    ::close(fd_);
    fd_ = -1;
  }
}

bool read_journal_raw(const std::string& path, std::vector<Input>& out, JournalReadResult& r) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  std::vector<unsigned char> data;
  std::array<unsigned char, 1 << 16> chunk{};
  for (;;) {
    const ssize_t n = ::read(fd, chunk.data(), chunk.size());
    if (n <= 0) break;
    data.insert(data.end(), chunk.begin(), chunk.begin() + n);
  }
  ::close(fd);
  std::size_t off = 0;
  while (off < data.size()) {
    if (data.size() - off < 8) {
      r.truncated_tail = true;
      break;
    }
    std::uint32_t len, crc;
    std::memcpy(&len, data.data() + off, 4);
    std::memcpy(&crc, data.data() + off + 4, 4);
    if (len != sizeof(Input)) {
      r.corrupt = true;
      r.corrupt_offset = off;
      break;
    }
    if (data.size() - off - 8 < len) {
      r.truncated_tail = true;
      break;
    }
    if (crc32c(data.data() + off + 8, len) != crc) {
      r.corrupt = true;
      r.corrupt_offset = off;
      break;
    }
    Input in;
    std::memcpy(&in, data.data() + off + 8, sizeof in);
    out.push_back(in);
    ++r.records;
    off += 8 + len;
  }
  return true;
}

}  // namespace matchbox
