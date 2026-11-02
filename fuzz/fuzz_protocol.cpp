// libFuzzer: framer + decoder on arbitrary bytes must never read out of bounds; decoded orders
// re-encode to an identical frame.
#include <cstdlib>
#include <cstring>
#include <vector>

#include "matchbox/net.hpp"

using namespace matchbox;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::size_t off = 0;
  while (off < size) {
    const std::size_t len = frame_length(data + off, size - off);
    if (len == 0 || len == SIZE_MAX) break;
    Decoded d;
    if (decode_frame(data + off, len, d) == DecodeStatus::Ok && d.type == MsgType::Cancel) {
      std::vector<unsigned char> b;
      put_cancel(b, d.input.client_tag, d.input.order_id);
      if (b.size() != len || std::memcmp(b.data(), data + off, len) != 0) std::abort();
    }
    off += len;
  }
  return 0;
}
