/*
 * Leid 2026.7
 */
#pragma once
#ifndef _A55A55AA_STREAM_PARSER_H_
#define _A55A55AA_STREAM_PARSER_H_
/*
 * A55A55AA Protocol Specification
 * 
 * | Field      | Length | Description |
 * |------------|--------|-------------|
 * | Frame Head | 2      | Fixed 0xA5 0x5A |
 * | Frame Len  | 2      | Big-endian, value is length of "payload + checksum + trailer" |
 * | Payload    | N      | Variable length |
 * | Checksum   | 1      | Checksum of header + len + payload |
 * | Trailer    | 2      | Fixed 0x55 0xAA |
 * 
 * Therefore:
 * - Length field value L >= 3 (1 checksum + 2 trailer)
 * - Total frame length = 4 + L
 * - After receiving, must verify last two bytes are 0x55 0xAA
*/
#include <sp/StreamParser.h>
#include <cstdint>

namespace sp {

class A55A55AAParser : public StreamParser {
public:
    /**
     * @param tag          Connection identifier
     * @param timeout_ms  Inter-frame timeout in milliseconds
     * @param max_frame    Maximum allowed frame length (prevent malicious lengths)
     */
    A55A55AAParser(const std::string& tag,
                   uint32_t timeout_ms = 300,
                   size_t max_frame = 10 * 1024)
        : StreamParser(tag, timeout_ms, max_frame)
    {}

    // Returns empty vector on failure; non-empty frame on success.
    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) override
    {
        // Protocol format:
        //   [0xA5 0x5A]  [LenHi LenLo]  [payload...]  [checksum]  [0x55 0xAA]
        //   2 bytes      2 bytes        N bytes        1 bytes     2 bytes
        // LenHi LenLo = payload.size() + 1 (checksum) + 2 (trailer occupies 2 bytes)

        const uint16_t checksum_trailer_len = 1 + 2;
        const size_t kMaxLenField = 0xFFFF;   // LenHi LenLo is 16 bit

        // payload.size() + trailer_len must fit in uint16_t (max 0xFFFF).
        // Check before the static_cast to avoid silent truncation. This limit is
        // private to the framing, so the transport cannot restate it and it is
        // reported here.
        if (payload.size() > kMaxLenField - checksum_trailer_len) {
            std::string detail = "build payload length too large: " + std::to_string(payload.size());
            reportError(ErrorCode::ProtocolError, detail);
            return {};
        }

        uint16_t L = static_cast<uint16_t>(payload.size()) + checksum_trailer_len;
        size_t total_len = 4 + L;  // 2 header + 2 len + payload + 3 trailer
        if (total_len > m_max_frame_size) {
            // The budget is the transport's own: it reports the refusal with the
            // payload size and max_frame_bytes, so nothing is reported here.
            return {};
        }

        std::vector<uint8_t> frame;
        frame.reserve(total_len);

        frame.push_back(0xA5);
        frame.push_back(0x5A);
        frame.push_back(static_cast<uint8_t>((L >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(L & 0xFF));
        frame.insert(frame.end(), payload.begin(), payload.end());
        uint8_t check_sum = getChecksum(frame.data(), total_len - 3);
        frame.push_back(check_sum);
        frame.push_back(0x55);
        frame.push_back(0xAA);

        return frame;
    }

protected:
    void processByte(uint8_t byte) override {
        switch (m_state) {
        //--------------------------------------------------
        // State 0: Wait for first byte of frame header 0xA5
        //--------------------------------------------------
        case 0:
            m_buf.clear();
            if (byte == 0xA5) {
                m_buf.push_back(byte);
                m_state = 1;
            }
            // Non-0xA5 bytes are discarded, stay in state 0 (auto resync)
            break;

        //--------------------------------------------------
        // State 1: Wait for second byte of frame header 0x5A
        //--------------------------------------------------
        case 1:
            m_buf.push_back(byte);
            if (byte == 0x5A) {
                m_state = 2;
            } else {
                // Frame header error, try resync
                std::string detail = "expect 0x5A after 0xA5, got 0x" + toHex(byte);
                reportError(ErrorCode::InvalidHeader, detail);
                reset();
                // Current byte may be a new 0xA5
                if (byte == 0xA5) {
                    m_buf.push_back(byte);
                    m_state = 1;
                }
            }
            break;

        //--------------------------------------------------
        // State 2: Receive length high byte (big-endian)
        //--------------------------------------------------
        case 2:
            m_buf.push_back(byte);
            m_state = 3;
            break;

        //--------------------------------------------------
        // State 3: Receive length low byte and calculate remaining bytes to receive
        //--------------------------------------------------
        case 3: {
            m_buf.push_back(byte);

            // Big-endian length parsing: L = payload + trailer
            uint16_t L = (static_cast<uint16_t>(m_buf[2]) << 8) |
                          static_cast<uint16_t>(m_buf[3]);

            // Validation check
            if (L < 3) {                    // At least 1 byte for checksum + 2 bytes for trailer required
                reportError(ErrorCode::ProtocolError, "length field < 3");
                reset();
                break;
            }
            if (4 + static_cast<size_t>(L) > m_max_frame_size) {             // Safety check
                reportError(ErrorCode::BufferOverflow,
                            "frame length too large: " + std::to_string(L));
                reset();
                break;
            }

            m_expected_len = L;             // Need to receive L more bytes
            m_state = 4;
            break;
        }

        //--------------------------------------------------
        // State 4: Receive payload + trailer
        //--------------------------------------------------
        case 4:
            m_buf.push_back(byte);

            // Received all bytes declared by length field
            if (m_buf.size() == 4 + m_expected_len) {
                // Verify trailer
                size_t tail_pos = m_buf.size() - 2;
                if (m_buf[tail_pos] == 0x55 && m_buf[tail_pos + 1] == 0xAA) {
                    uint8_t check_sum = getChecksum(m_buf.data(), m_buf.size() - 3);
                    if (check_sum != m_buf[m_buf.size() - 3]) {
                        std::string detail = "checksum mismatch: expected 0x" +
                            toHex(check_sum) + ", got 0x" +
                            toHex(m_buf[m_buf.size() - 3]);
                        reportError(ErrorCode::ChecksumFail, detail);
                        reset();
                        break;
                    }
                    completeFrame(4, m_expected_len - 3);         // Success -> callback + auto reset
                } else {
                    std::string detail = "invalid tail: expected 0x55 0xAA, got 0x" +
                        toHex(m_buf[tail_pos]) + " 0x" + toHex(m_buf[tail_pos + 1]);
                    reportError(ErrorCode::InvalidTailer, detail);
                    reset();
                    // Try treating current byte as new frame header for resync
                    if (byte == 0xA5) {
                        m_buf.push_back(byte);
                        m_state = 1;
                    }
                }
            } else {
                // Still receiving payload + trailer, stay in state 4
            }
            break;

        //--------------------------------------------------
        default:
            reset();
            break;
        }
    }

    static char getChecksum(const uint8_t* data, size_t len) {
        char checksum = 0;
        for (size_t i = 0; i < len; i++) {
            checksum ^= *data++;
        }
        return checksum;
    }
};

}  // namespace sp

#endif  // !_A55A55AA_STREAM_PARSER_H_
