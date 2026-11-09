/*
 * Leid 2026.7
 *
 * Tests for the A55A55AA example parser (examples/A55A55AAParser.h). The file
 * is an example -- a downstream copies it -- so what is tested here is exactly
 * what a downstream gets: the encoder and decoder are inverses, a corrupted
 * frame is reported once and the parser resynchronises, and a frame that does
 * not fit the budget is refused without a second report.
 */
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <sp/StreamParser.h>

#include "A55A55AAParser.h"

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
std::vector<sp::StreamParser::ErrorCode> g_errors;

void onFrame(const std::string&, const uint8_t* data, size_t len) {
    g_frames.emplace_back(reinterpret_cast<const char*>(data), len);
}

void onError(const std::string&, sp::StreamParser::ErrorCode code, const std::string&) {
    g_errors.push_back(code);
}

void reset_capture() {
    g_frames.clear();
    g_errors.clear();
}

std::vector<uint8_t> bytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

void test_round_trip() {
    reset_capture();
    sp::A55A55AAParser parser("a55");
    parser.setFrameCallback(onFrame);

    const std::vector<uint8_t> frame = parser.buildFrame(bytes("hello"));
    check(frame.size() == 12, "buildFrame() produces 4 + payload + 3 bytes");
    check(frame.size() >= 4 && frame[0] == 0xA5 && frame[1] == 0x5A, "the frame starts with A5 5A");
    check(frame.size() >= 4 && frame[2] == 0x00 && frame[3] == 0x08,
          "the length field is big endian: payload + checksum + trailer");
    check(frame.size() >= 3 && frame[frame.size() - 2] == 0x55 && frame[frame.size() - 1] == 0xAA,
          "the frame ends with 55 AA");

    parser.feed(frame.data(), frame.size());
    check(g_frames.size() == 1 && g_frames[0] == "hello",
          "feeding the built frame delivers the original payload");

    reset_capture();
    for (uint8_t byte : frame) {
        parser.feed(&byte, 1);
    }
    check(g_frames.size() == 1 && g_frames[0] == "hello",
          "a frame fed one byte at a time arrives the same way");
}

void test_checksum_failure_resyncs() {
    reset_capture();
    sp::A55A55AAParser parser("a55");
    parser.setFrameCallback(onFrame);
    parser.setErrorCallback(onError);

    std::vector<uint8_t> frame = parser.buildFrame(bytes("abc"));
    frame[5] ^= 0xFF;  // corrupt a payload byte, leave the checksum alone
    parser.feed(frame.data(), frame.size());

    check(g_frames.empty(), "a frame with a bad checksum is not delivered");
    check(g_errors.size() == 1 && g_errors[0] == sp::StreamParser::ErrorCode::ChecksumFail,
          "the failure is reported as ErrorCode::ChecksumFail");

    reset_capture();
    const std::vector<uint8_t> good = parser.buildFrame(bytes("ok"));
    parser.feed(good.data(), good.size());
    check(g_frames.size() == 1 && g_frames[0] == "ok",
          "the parser resynchronises and decodes the next frame");
}

void test_invalid_trailer() {
    reset_capture();
    sp::A55A55AAParser parser("a55");
    parser.setFrameCallback(onFrame);
    parser.setErrorCallback(onError);

    std::vector<uint8_t> frame = parser.buildFrame(bytes("x"));
    frame[frame.size() - 1] = 0xAB;
    parser.feed(frame.data(), frame.size());

    check(g_frames.empty(), "a frame with a wrong trailer is not delivered");
    check(g_errors.size() == 1 && g_errors[0] == sp::StreamParser::ErrorCode::InvalidTailer,
          "the failure is reported as ErrorCode::InvalidTailer");
}

void test_invalid_header_and_bad_length() {
    reset_capture();
    sp::A55A55AAParser parser("a55");
    parser.setErrorCallback(onError);

    const std::vector<uint8_t> bad_header = {0xA5, 0x00};
    parser.feed(bad_header.data(), bad_header.size());
    check(g_errors.size() == 1 && g_errors[0] == sp::StreamParser::ErrorCode::InvalidHeader,
          "A5 followed by something else is ErrorCode::InvalidHeader");

    reset_capture();
    const std::vector<uint8_t> short_length = {0xA5, 0x5A, 0x00, 0x02};
    parser.feed(short_length.data(), short_length.size());
    check(g_errors.size() == 1 && g_errors[0] == sp::StreamParser::ErrorCode::ProtocolError,
          "a length smaller than the trailer is ErrorCode::ProtocolError");

    reset_capture();
    const std::vector<uint8_t> huge_length = {0xA5, 0x5A, 0x4E, 0x20};  // 20000 > 10 KiB
    parser.feed(huge_length.data(), huge_length.size());
    check(g_errors.size() == 1 && g_errors[0] == sp::StreamParser::ErrorCode::BufferOverflow,
          "a length past the frame budget is ErrorCode::BufferOverflow");
}

void test_build_refusals() {
    reset_capture();
    sp::A55A55AAParser parser("a55");
    parser.setErrorCallback(onError);

    // The 16 bit length field cannot express payload + 3 any more: this limit is
    // the framing's own, so the parser reports it.
    const std::vector<uint8_t> too_large(0xFFFF - 2, 0x00);
    const std::vector<uint8_t> refused = parser.buildFrame(too_large);
    check(refused.empty(), "a payload that overflows the length field is refused");
    check(g_errors.size() == 1 && g_errors[0] == sp::StreamParser::ErrorCode::ProtocolError,
          "that refusal is reported as ErrorCode::ProtocolError");

    // The frame budget belongs to the transport, which reports it: take a parser
    // with a small budget and check that nothing is reported here.
    reset_capture();
    sp::A55A55AAParser small("a55", 300, 16);
    small.setErrorCallback(onError);
    const std::vector<uint8_t> over_budget = small.buildFrame(std::vector<uint8_t>(20, 0x00));
    check(over_budget.empty(), "a payload past the frame budget is refused");
    check(g_errors.empty(), "and nothing is reported: the transport owns that budget");
}

}  // namespace

int main() {
    test_round_trip();
    test_checksum_failure_resyncs();
    test_invalid_trailer();
    test_invalid_header_and_bad_length();
    test_build_refusals();

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all checks passed" << std::endl;
    return 0;
}
