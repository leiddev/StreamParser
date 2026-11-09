/*
 * Leid 2026.7
 *
 * StartEndParser: framing used by TcpServerElement (the legacy "document push"
 * channel), that is  0x02 <payload> 0x00 0x00 0x03.
 *
 * The frame callback receives the payload only: the start byte and the
 * terminator are stripped, so it sees exactly what buildFrame() was given.
 *
 * The channel is send-only from the application point of view: the parser is
 * bound to every accepted session because sp::TcpSessionParserAdapter requires
 * one, and inbound data is only ever used for logging / diagnostics.
 */
#pragma once
#ifndef _START_END_PARSER_H_
#define _START_END_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <sp/StreamParser.h>

namespace sp {

class StartEndParser : public StreamParser {
public:
    /* Enumerators instead of static constexpr members: this header is the whole
       implementation, and in C++14 a static constexpr member that is passed by
       reference (push_back, operator==) needs an out-of-class definition that a
       header cannot provide. */
    enum : std::uint8_t { kStartOfFrame = 0x02, kEndOfFrame = 0x03 };
    // 0x02 + payload + 0x00 0x00 0x03
    static constexpr std::size_t kOverhead = 4;

    explicit StartEndParser(const std::string &tag,
                            std::uint32_t timeout_ms = 300,
                            std::size_t max_frame_size = 1024 * 1024)
        : StreamParser(tag, timeout_ms, max_frame_size) {}

    // Wrap a payload into a complete on-wire frame. Returns an empty vector when
    // the payload does not fit into the configured frame budget: that budget
    // belongs to the transport, which reports the refusal, so nothing is reported
    // here (see StreamParser::buildFrame).
    std::vector<std::uint8_t> buildFrame(const std::vector<std::uint8_t> &payload) override {
        std::vector<std::uint8_t> frame;
        if (payload.size() + kOverhead > m_max_frame_size) {
            // max_frame_bytes is the transport's own budget and it reports the
            // refusal there, so staying silent keeps one failure to one reporter.
            return frame;
        }
        frame.reserve(payload.size() + kOverhead);
        frame.push_back(kStartOfFrame);
        frame.insert(frame.end(), payload.begin(), payload.end());
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(kEndOfFrame);
        return frame;
    }

protected:
    void processByte(std::uint8_t byte) override {
        if (m_state == 0) {
            if (byte != kStartOfFrame) {
                return;  // idle: drop noise
            }
            m_buf.clear();
            m_buf.push_back(byte);
            m_state = 1;
            updateActivity();
            return;
        }

        m_buf.push_back(byte);
        const std::size_t n = m_buf.size();
        if (n >= kOverhead &&
            m_buf[n - 3] == 0x00 && m_buf[n - 2] == 0x00 && m_buf[n - 1] == kEndOfFrame) {
            // Deliver the payload only, like the other parsers do: the leading
            // 0x02 and the 00 00 03 terminator are framing, not content.
            completeFrame(1, n - kOverhead);
            return;
        }
        updateActivity();
    }
};

}  // namespace sp

#endif  // !_START_END_PARSER_H_
