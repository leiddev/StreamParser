/*
 * A downstream that consumes the release the way the README tells it to:
 * find_package(sp) and link sp::transport. It includes one core header, one
 * transport header (which pulls asio) and the generated version header, so the
 * installed package has to carry all three, and it derives from StreamParser --
 * the extension point the library exists for -- so the static library has to
 * link as well.
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <sp/StreamParser.h>
#include <sp/Transport.h>
#include <sp/version.hpp>

namespace {

class ConsumerParser : public sp::StreamParser {
 public:
  ConsumerParser() : sp::StreamParser("consumer", 1000, 4096) {}

 protected:
  void processByte(uint8_t) override {}

  std::vector<uint8_t> buildFrame(const std::vector<uint8_t> &payload) override {
    return payload;
  }
};

}  // namespace

int main() {
  ConsumerParser parser;
  int frames = 0;
  parser.setFrameCallback(
      [&frames](const std::string &, const uint8_t *, size_t) { ++frames; });
  parser.feed("abc", 3);

  std::printf("sp %s, transport kind %d, frames %d\n", sp::version(),
              static_cast<int>(sp::TransportKind::TcpServer), frames);
  return frames == 0 ? 0 : 1;
}
