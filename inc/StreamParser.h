/*
 * Leid 2026.7
 */
#pragma once
#ifndef _STREAM_PARSER_H_
#define _STREAM_PARSER_H_

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace sp {

class StreamParser {
public:
    // Complete frame callback
    using FrameCallback = std::function<void(const std::string& tag,
                                             const uint8_t* data,
                                             size_t len)>;

    // Error callback
    enum class ErrorCode {
        Timeout,            // Frame receive timeout
        InvalidHeader,      // Invalid frame header
        InvalidTailer,      // Invalid frame trailer
        ChecksumFail,       // Checksum mismatch
        BufferOverflow,     // Buffer overflow
        ProtocolError       // Other protocol error
    };
    using ErrorCallback = std::function<void(const std::string& tag,
                                             ErrorCode code,
                                             const std::string& detail)>;

    StreamParser(const std::string& tag, uint32_t timeout_ms, size_t max_frame_size = 512);
    virtual ~StreamParser() = default;

    void setFrameCallback(FrameCallback cb);
    void setErrorCallback(ErrorCallback cb);

    // Core interface: feed data of any length (auto handles sticky/partial packets)
    void feed(const char* data, size_t size);
    void feed(const uint8_t* data, size_t size);

    // Called periodically by external timer (recommended 20~50ms)
    void checkTimeout();

    // Call when connection is disconnected or needs forced cleanup.
    //
    // CONTRACT: reset() only clears protocol-internal state - buffered bytes,
    // state machine position, timeouts, etc. It MUST NOT touch the callbacks
    // previously installed via setFrameCallback() / setErrorCallback(); those
    // remain bound to the lifetime of the StreamParser object so that business
    // code can register them once and have them survive adapter close()/open()
    // cycles.
    void reset();

    const std::string& tag() const { return m_tag; }

    // Encode a payload into a complete protocol frame.
    // Inverse operation of processByte(): feed() decodes inbound bytes,
    // buildFrame() encodes an outbound payload into a transmittable byte stream.
    // Return non-empty frame on success; empty vector on protocol-level failure
    // (e.g. payload too large). Empty means the frame is invalid, do not send.
    //
    // Report a failure with reportError() only when the reason is private to the
    // protocol and the transport cannot restate it, i.e. a limit encoded in the
    // framing itself. A payload that merely does not fit max_frame_bytes is the
    // transport's own budget: return an empty vector silently and let it report
    // the refusal, so one failure never has two reporters.
    virtual std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) = 0;

protected:
    // ========== Must be implemented by derived class ==========
    // Called once per received byte. Derived class maintains its own state machine here.
    // Call completeFrame() when a complete frame is determined; call reportError() on errors.
    virtual void processByte(uint8_t byte) = 0;

    // ========== Helper functions for derived class ==========
    void completeFrame();                           // Notify upper layer of frame completion, reset state
    void completeFrame(size_t offset, size_t size); // Notify upper layer of frame completion with offset and size, reset state
    void reportError(ErrorCode code, const std::string& detail = "");
    void updateActivity();                          // Update timestamp when new data arrives
    bool isTimeout() const;                         // Check if currently in timeout state

    // Public buffer and state (read/write directly by derived class)
    std::vector<uint8_t> m_buf;                     // Frame being assembled
    int                  m_state = 0;               // Protocol state machine
    size_t               m_expected_len = 0;        // Used for variable-length protocols (optional)
    size_t               m_max_frame_size;          // Maximum allowed frame length

    // Helper functions
    static std::string toHex(uint8_t byte);         // Convert single byte to hex string (e.g. "A5")

private:
    std::string          m_tag;
    uint32_t             m_timeout_ms;
    FrameCallback        m_frame_cb;
    ErrorCallback        m_error_cb;
    std::chrono::steady_clock::time_point m_last_activity;
    bool                 m_in_frame = false;        // Whether currently in mid-frame state
};

}  // namespace sp

#endif  // !_STREAM_PARSER_H_
