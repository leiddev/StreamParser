/*
 * Leid 2026.7
 *
 * Tests for the StartEnd example parser (examples/StartEndParser.h), the
 * 0x02 <payload> 0x00 0x00 0x03 framing used for the document push channel.
 *
 * The payload is delivered without the start byte and terminator, which is the
 * property the element code depends on: what goes into buildFrame() is what
 * comes out of the frame callback.
 */
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <sp/StreamParser.h>

#include "StartEndParser.h"

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

void onFrame(const std::string&, const uint8_t* data, size_t len) {
    g_frames.emplace_back(reinterpret_cast<const char*>(data), len);
}

std::vector<uint8_t> bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

void test_round_trip() {
    g_frames.clear();
    sp::StartEndParser parser("doc");
    parser.setFrameCallback(onFrame);

    const std::vector<uint8_t> frame = parser.buildFrame(bytes("ab"));
    check(frame.size() == 6, "buildFrame() is start + payload + 00 00 03");
    check(frame.size() == 6 && frame[0] == 0x02 && frame[3] == 0x00 && frame[4] == 0x00 &&
              frame[5] == 0x03,
          "the frame is 0x02 <payload> 0x00 0x00 0x03");

    parser.feed(frame.data(), frame.size());
    check(g_frames.size() == 1 && g_frames[0] == "ab",
          "the callback sees the payload without the framing bytes");
}

void test_byte_by_byte_and_noise() {
    g_frames.clear();
    sp::StartEndParser parser("doc");
    parser.setFrameCallback(onFrame);

    const std::vector<uint8_t> frame = parser.buildFrame(bytes("hello"));
    // Noise that does not contain 0x02: the parser takes the first 0x02 it sees
    // as a start byte, so a 0x02 in the noise would open a frame of its own.
    parser.feed("\x01\x00\x05\x04", 4);
    for (uint8_t byte : frame) {
        parser.feed(&byte, 1);
    }
    check(g_frames.size() == 1 && g_frames[0] == "hello",
          "noise is dropped and a byte-by-byte frame still arrives");
}

void test_sticky_frames() {
    g_frames.clear();
    sp::StartEndParser parser("doc");
    parser.setFrameCallback(onFrame);

    std::vector<uint8_t> both = parser.buildFrame(bytes("one"));
    const std::vector<uint8_t> second = parser.buildFrame(bytes("two"));
    both.insert(both.end(), second.begin(), second.end());
    parser.feed(both.data(), both.size());

    check(g_frames.size() == 2, "two frames in one feed are both delivered");
    check(g_frames.size() == 2 && g_frames[0] == "one" && g_frames[1] == "two",
          "sticky frames keep their order and payloads");
}

void test_budget_refusal_is_silent() {
    std::vector<sp::StreamParser::ErrorCode> errors;
    sp::StartEndParser parser("doc", 300, 8);
    parser.setErrorCallback([&errors](const std::string&, sp::StreamParser::ErrorCode code,
                                      const std::string&) { errors.push_back(code); });

    const std::vector<uint8_t> refused = parser.buildFrame(bytes("0123456789"));
    check(refused.empty(), "a payload past the budget is refused");
    check(errors.empty(), "and nothing is reported: the transport owns that budget");
}

void test_payload_containing_the_terminator() {
    g_frames.clear();
    sp::StartEndParser parser("doc");
    parser.setFrameCallback(onFrame);

    // The terminator is not escaped by the framing, so a payload that contains
    // 00 00 03 ends the frame early. Documented here so the limitation is a
    // decision rather than a surprise.
    const std::vector<uint8_t> frame = parser.buildFrame({0x00, 0x00, 0x03});
    parser.feed(frame.data(), frame.size());

    check(g_frames.size() == 1 && g_frames[0].empty(),
          "a payload containing 00 00 03 ends the frame early (terminator is not escaped)");
}

}  // namespace

int main() {
    test_round_trip();
    test_byte_by_byte_and_noise();
    test_sticky_frames();
    test_budget_refusal_is_silent();
    test_payload_containing_the_terminator();

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all checks passed" << std::endl;
    return 0;
}
