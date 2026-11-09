/*
 * Leid 2026.7
 *
 * Unit tests for the parsing core itself (sp::StreamParser, sp/StreamParser.h).
 * No sockets, no timers beyond a sleep, no example parser: the point is the
 * behaviour every derived parser gets for free, and the contract the header
 * promises -- sticky and partial packets, reset() keeping the callbacks,
 * checkTimeout() only while a frame is open, and the buffer guard.
 */
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sp/StreamParser.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "[OK]   " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

std::vector<std::string> g_frames;
std::vector<std::pair<sp::StreamParser::ErrorCode, std::string>> g_errors;

void onFrame(const std::string&, const uint8_t* data, size_t len) {
    g_frames.emplace_back(reinterpret_cast<const char*>(data), len);
}

void onError(const std::string&, sp::StreamParser::ErrorCode code, const std::string& detail) {
    g_errors.emplace_back(code, detail);
}

void reset_capture() {
    g_frames.clear();
    g_errors.clear();
}

// 0x7E <len> <payload...>: the smallest protocol with a variable length, enough
// to drive the base class without borrowing an example.
class TestParser : public sp::StreamParser {
public:
    TestParser(uint32_t timeout_ms = 300, size_t max_frame_size = 64)
        : sp::StreamParser("test", timeout_ms, max_frame_size) {}

    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) override {
        std::vector<uint8_t> frame;
        frame.push_back(0x7E);
        frame.push_back(static_cast<uint8_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());
        return frame;
    }

protected:
    void processByte(uint8_t byte) override {
        if (m_state == 0) {
            if (byte != 0x7E) {
                return;
            }
            m_buf.clear();
            m_buf.push_back(byte);
            m_state = 1;
            return;
        }
        m_buf.push_back(byte);
        if (m_state == 1) {
            m_expected_len = byte;
            m_state = 2;
            return;
        }
        if (m_buf.size() == 2 + m_expected_len) {
            completeFrame(2, m_expected_len);
        }
    }
};

// Never completes a frame, so the base class buffer guard is what has to stop it.
class NeverCompletesParser : public sp::StreamParser {
public:
    NeverCompletesParser(size_t max_frame_size)
        : sp::StreamParser("sink", 300, max_frame_size) {}

    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>&) override { return {}; }

protected:
    void processByte(uint8_t byte) override { m_buf.push_back(byte); }
};

// Asks for a frame window that runs past the end of the buffer.
class BadOffsetParser : public sp::StreamParser {
public:
    BadOffsetParser() : sp::StreamParser("bad", 300, 64) {}

    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>&) override { return {}; }

protected:
    void processByte(uint8_t) override {
        m_buf.push_back(0x00);
        completeFrame(m_buf.size() + 8, 1);
    }
};

void test_partial_and_sticky() {
    reset_capture();
    TestParser parser;
    parser.setFrameCallback(onFrame);

    const std::vector<uint8_t> frame = parser.buildFrame({'h', 'i'});
    for (uint8_t byte : frame) {
        parser.feed(&byte, 1);
    }
    check(g_frames.size() == 1, "a frame fed one byte at a time arrives once");
    check(g_frames.size() == 1 && g_frames[0] == "hi", "the payload survives a byte-by-byte feed");

    reset_capture();
    std::vector<uint8_t> two = parser.buildFrame({'a'});
    const std::vector<uint8_t> second = parser.buildFrame({'b', 'b'});
    two.insert(two.end(), second.begin(), second.end());
    parser.feed(two.data(), two.size());
    check(g_frames.size() == 2, "two frames in one feed are both delivered");
    check(g_frames.size() == 2 && g_frames[0] == "a" && g_frames[1] == "bb",
          "sticky packets keep their order and payloads");
}

void test_noise_between_frames() {
    reset_capture();
    TestParser parser;
    parser.setFrameCallback(onFrame);

    const std::vector<uint8_t> frame = parser.buildFrame({'x'});
    std::vector<uint8_t> noisy = {0x00, 0xFF, 0x11};
    noisy.insert(noisy.end(), frame.begin(), frame.end());
    noisy.push_back(0x33);
    parser.feed(noisy.data(), noisy.size());

    check(g_frames.size() == 1, "bytes outside a frame are dropped");
    check(g_frames.size() == 1 && g_frames[0] == "x", "the frame inside the noise is still delivered");
}

void test_reset_keeps_callbacks() {
    reset_capture();
    TestParser parser;
    parser.setFrameCallback(onFrame);
    parser.setErrorCallback(onError);

    const uint8_t half[] = {0x7E, 0x05, 'a', 'b'};  // half of a frame
    parser.feed(half, sizeof(half));
    parser.reset();
    check(g_frames.empty(), "reset() drops the partly assembled frame");

    const std::vector<uint8_t> frame = parser.buildFrame({'z'});
    parser.feed(frame.data(), frame.size());
    check(g_frames.size() == 1 && g_frames[0] == "z",
          "callbacks survive reset(): the frame after reset still arrives");
}

void test_timeout() {
    reset_capture();
    TestParser parser(20);
    parser.setErrorCallback(onError);

    const uint8_t half[] = {0x7E, 0x05, 'a', 'b'};
    parser.feed(half, sizeof(half));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    parser.checkTimeout();
    check(g_errors.size() == 1, "an open frame times out");
    check(g_errors.size() == 1 && g_errors[0].first == sp::StreamParser::ErrorCode::Timeout,
          "the timeout is reported as ErrorCode::Timeout");

    // After the timeout the parser is idle, and idle does not time out again.
    reset_capture();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    parser.checkTimeout();
    check(g_errors.empty(), "an idle parser does not time out");
}

void test_buffer_guard() {
    reset_capture();
    NeverCompletesParser parser(16);
    parser.setErrorCallback(onError);

    const std::vector<uint8_t> flood(64, 0x41);
    parser.feed(flood.data(), flood.size());

    check(!g_errors.empty(), "a frame that never ends is stopped by the buffer guard");
    check(!g_errors.empty() && g_errors[0].first == sp::StreamParser::ErrorCode::BufferOverflow,
          "the guard reports ErrorCode::BufferOverflow");
}

void test_bad_frame_window() {
    reset_capture();
    // The window is only validated when a frame callback is installed -- the
    // check exists to protect the callback -- so install one.
    BadOffsetParser parser;
    parser.setFrameCallback(onFrame);
    parser.setErrorCallback(onError);

    const uint8_t byte = 0x00;
    parser.feed(&byte, 1);

    check(g_errors.size() == 1, "a frame window past the buffer is refused");
    check(g_errors.size() == 1 && g_errors[0].first == sp::StreamParser::ErrorCode::BufferOverflow,
          "the refusal is reported as ErrorCode::BufferOverflow");
}

void test_no_callbacks_and_tag() {
    TestParser parser;
    const std::vector<uint8_t> frame = parser.buildFrame({'q'});
    parser.feed(frame.data(), frame.size());
    check(true, "feeding without callbacks does not crash");
    check(parser.tag() == "test", "tag() returns the tag the parser was built with");
}

}  // namespace

int main() {
    test_partial_and_sticky();
    test_noise_between_frames();
    test_reset_keeps_callbacks();
    test_timeout();
    test_buffer_guard();
    test_bad_frame_window();
    test_no_callbacks_and_tag();

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all checks passed" << std::endl;
    return 0;
}
