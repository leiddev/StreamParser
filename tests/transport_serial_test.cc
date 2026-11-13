/*
 * Serial cases for the Transport facade (sp::Transport).
 *
 * Two groups:
 *
 *  - The missing-device paths run everywhere. A device that is not there must
 *    not make start() fail: the README promises the transport keeps retrying
 *    in the background, which is what a hot-pluggable collector needs. An
 *    unconfigured device is the one serial case that is fatal.
 *
 *  - The round-trip cases need a real pair of devices (or a pty pair). They
 *    are enabled by setting SP_TEST_SERIAL_PORT and SP_TEST_SERIAL_PORT_PEER,
 *    for example "COM1" and "COM2", or two ptys from socat. Without them the
 *    group prints SKIP and the test still passes, so CI needs no wiring.
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/TransportTestKit.h"

namespace {

using sptest::bytes;
using sptest::ErrorEvent;
using sptest::FakeParser;
using sptest::Frame;
using sptest::kWaitMs;
using sptest::Recorder;
using sptest::sleep_ms;

int g_failures = 0;
int g_skipped  = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "[OK]   " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

void skip(const char* what) {
    std::cout << "[SKIP] " << what << std::endl;
    ++g_skipped;
}

// Error codes the facade reports for serial, written out here so a change in
// the implementation shows up as a failing test.
constexpr int kNotConfigured = -3;
constexpr int kCannotOpen     = -4;

bool has_error(Recorder& rec, int code) {
    for (const ErrorEvent& e : rec.errors()) {
        if (e.code == code) {
            return true;
        }
    }
    return false;
}

std::string error_detail(Recorder& rec, int code) {
    for (const ErrorEvent& e : rec.errors()) {
        if (e.code == code) {
            return e.detail;
        }
    }
    return std::string();
}

sp::TransportConfig serial_config(const std::string& device) {
    sp::TransportConfig cfg;
    cfg.kind      = sp::TransportKind::Serial;
    cfg.name      = "serial";
    cfg.device    = device;
    cfg.baud_rate = 115200;
    return cfg;
}

struct Facade {
    Recorder                       rec;
    std::unique_ptr<sp::Transport> transport;

    ~Facade() {
        if (transport) {
            transport->stop();
        }
    }

    bool build(const sp::TransportConfig& cfg) {
        transport = sp::CreateTransport(
            cfg, sptest::fake_factory(cfg.frame_timeout_ms, cfg.max_frame_bytes), rec.handler());
        return transport != nullptr;
    }
};

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

// -------------------------------------------------
// Missing-device paths (every platform)
// -------------------------------------------------
void test_unconfigured_device_is_fatal() {
    Facade f;
    check(f.build(serial_config("")), "serial: transport built with an empty device");
    check(!f.transport->start(), "serial: an empty device makes start() fail");
    check(has_error(f.rec, kNotConfigured), "serial: an empty device is reported with code -3");
    check(!f.transport->is_running(), "serial: is_running() is false after the failure");
}

void test_missing_device_keeps_retrying() {
    const std::string device =
        env("SP_TEST_SERIAL_MISSING").empty() ? std::string("SP_NO_SUCH_DEVICE")
                                             : env("SP_TEST_SERIAL_MISSING");

    sp::TransportConfig cfg = serial_config(device);
    cfg.reopen_delay_ms     = 100;

    Facade f;
    check(f.build(cfg), "serial: transport built for a missing device");
    check(f.transport->start(),
          "serial: a missing device does not make start() fail, it retries");
    check(f.transport->is_running(), "serial: is_running() is true while the device is missing");
    check(f.rec.wait_error_code(kCannotOpen),
          "serial: a missing device is reported with code -4");
    check(error_detail(f.rec, kCannotOpen).find(device) != std::string::npos,
          "serial: the open failure names the device");

    // The open is retried, so the same error arrives more than once.
    const std::size_t before = f.rec.error_count();
    sleep_ms(400);
    check(f.rec.error_count() > before,
          "serial: the open is retried on its own after reopen_delay_ms");
    check(f.transport->links().empty(), "serial: a missing device has no links");
    check(!f.transport->send(device, bytes("x")), "serial: send() with no link returns false");
}

// -------------------------------------------------
// Round trip (needs a real device pair)
// -------------------------------------------------
void test_roundtrip(const std::string& mine, const std::string& peer_device) {
    sp::TransportConfig mine_cfg = serial_config(mine);
    mine_cfg.frame_timeout_ms    = 500;

    // Both ends speak the same test framing, so each can read the other.
    sp::TransportConfig peer_cfg = serial_config(peer_device);
    peer_cfg.frame_timeout_ms    = 500;

    Recorder mine_rec;
    Recorder peer_rec;

    std::unique_ptr<sp::Transport> mine_t = sp::CreateTransport(
        mine_cfg, sptest::fake_factory(mine_cfg.frame_timeout_ms, mine_cfg.max_frame_bytes),
        mine_rec.handler());
    std::unique_ptr<sp::Transport> peer_t = sp::CreateTransport(
        peer_cfg, sptest::fake_factory(peer_cfg.frame_timeout_ms, peer_cfg.max_frame_bytes),
        peer_rec.handler());

    check(mine_t != nullptr && peer_t != nullptr, "serial: both ends of the pair are built");
    if (!mine_t || !peer_t) {
        return;
    }
    // Open the far end first: bytes written into a closed port are lost.
    const bool peer_up = peer_t->start();
    check(peer_up, "serial: the far end opens on the peer device");
    if (!peer_up) {
        std::cout << "[INFO] could not open " << peer_device << ", skipping the round trip"
                  << std::endl;
        return;
    }

    const bool mine_up = mine_t->start();
    check(mine_up, "serial: the near end opens on its device");
    if (!mine_up) {
        std::cout << "[INFO] could not open " << mine << ", skipping the round trip" << std::endl;
        peer_t->stop();
        return;
    }

    check(mine_rec.wait_link_ups(1),
          "serial: opening the device announces link_up");
    check(peer_rec.wait_link_ups(1),
          "serial: the far end also announces link_up");
    check(mine_t->links().size() == 1 && mine_t->links()[0] == mine,
          "serial: the only link is tagged with the device name");

    // Near end -> far end.
    check(mine_t->send(mine, bytes("ping")), "serial: send() on the open device returns true");
    check(peer_rec.wait_frames(1),
          "serial: the far end receives the frame");
    {
        const std::vector<Frame> frames = peer_rec.frames();
        check(frames.size() == 1 && sptest::as_string(frames[0].payload) == "ping",
              "serial: the payload crosses the pair intact");
    }

    // Far end -> near end, so the pair is proven in both directions.
    peer_t->send(peer_device, bytes("pong"));
    check(mine_rec.wait_frames(1),
          "serial: the near end receives the reply");
    {
        const std::vector<Frame> frames = mine_rec.frames();
        check(frames.size() == 1 && sptest::as_string(frames[0].payload) == "pong",
              "serial: the reply's payload is intact");
    }

    // Two frames back to back: a serial line is a byte stream like TCP, so the
    // parser has to split them.
    mine_rec.clear();
    peer_t->send(peer_device, bytes("one"));
    peer_t->send(peer_device, bytes("two"));
    check(mine_rec.wait_frames(2),
          "serial: two frames sent back to back are both delivered");
    {
        const std::vector<Frame> frames = mine_rec.frames();
        check(frames.size() == 2 && sptest::as_string(frames[0].payload) == "one" &&
                  sptest::as_string(frames[1].payload) == "two",
              "serial: back-to-back frames keep their order");
    }

    mine_t->stop();
    peer_t->stop();
    check(!mine_t->is_running() && !peer_t->is_running(), "serial: both ends stop cleanly");
}

}  // namespace

int main() {
    std::cout << "== missing device ==" << std::endl;
    test_unconfigured_device_is_fatal();
    test_missing_device_keeps_retrying();

    std::cout << "== round trip ==" << std::endl;
    const std::string mine = env("SP_TEST_SERIAL_PORT");
    const std::string peer = env("SP_TEST_SERIAL_PORT_PEER");
    if (mine.empty() || peer.empty()) {
        skip("SP_TEST_SERIAL_PORT / SP_TEST_SERIAL_PORT_PEER not set, round trip not run");
    } else {
        test_roundtrip(mine, peer);
    }

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    if (g_skipped != 0) {
        std::cout << "all checks passed (" << g_skipped << " group(s) skipped)" << std::endl;
    } else {
        std::cout << "all checks passed" << std::endl;
    }
    return 0;
}
