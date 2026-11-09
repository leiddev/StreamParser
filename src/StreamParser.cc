/*
 * Leid 2026.7
 */
#include <sp/StreamParser.h>

namespace sp {

StreamParser::StreamParser(const std::string& tag, uint32_t timeout_ms, size_t max_frame_size)
    : m_max_frame_size(max_frame_size)   // declaration order: m_max_frame_size comes first
    , m_tag(tag)
    , m_timeout_ms(timeout_ms)
{
    m_buf.reserve(max_frame_size);
    m_last_activity = std::chrono::steady_clock::now();
}

void StreamParser::setFrameCallback(FrameCallback cb) {
    m_frame_cb = std::move(cb);
}

void StreamParser::setErrorCallback(ErrorCallback cb) {
    m_error_cb = std::move(cb);
}

void StreamParser::feed(const char* data, size_t size) {
    feed(reinterpret_cast<const uint8_t*>(data), size);
}

void StreamParser::feed(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;

    updateActivity();
    m_in_frame = true;          // Any incoming data indicates we may be in a frame

    for (size_t i = 0; i < size; ++i) {
        // Prevent buffer from growing indefinitely due to malicious data
        if (m_buf.size() >= m_max_frame_size) {
            reportError(ErrorCode::BufferOverflow, "frame too large");
            reset();
            continue;
        }
        processByte(data[i]);   // Delegate to derived class state machine
    }
}

void StreamParser::checkTimeout() {
    if (!m_in_frame || m_state == 0) return;   // Idle state does not timeout

    if (isTimeout()) {
        reportError(ErrorCode::Timeout, "frame receive timeout");
        reset();
    }
}

void StreamParser::reset() {
    m_buf.clear();
    m_state = 0;
    m_expected_len = 0;
    m_in_frame = false;
    // Note: do not update m_last_activity, let next feed restart the timer
}

void StreamParser::completeFrame() {
    if (m_frame_cb && !m_buf.empty()) {
        m_frame_cb(m_tag, m_buf.data(), m_buf.size());
    }
    // Auto return to idle after completion, continue processing subsequent sticky packets
    m_buf.clear();
    m_state = 0;
    m_expected_len = 0;
    m_in_frame = false;
}

void StreamParser::completeFrame(size_t offset, size_t size) {
    if (m_frame_cb && !m_buf.empty()) {
        if (offset + size > m_buf.size()) {
            reportError(ErrorCode::BufferOverflow, "completeFrame offset+size exceeds buffer");
            reset();
            return;
        }
        m_frame_cb(m_tag, m_buf.data() + offset, size);
    }
    // Auto return to idle after completion, continue processing subsequent sticky packets
    m_buf.clear();
    m_state = 0;
    m_expected_len = 0;
    m_in_frame = false;
}

void StreamParser::reportError(ErrorCode code, const std::string& detail) {
    if (m_error_cb) {
        m_error_cb(m_tag, code, detail);
    }
}

void StreamParser::updateActivity() {
    m_last_activity = std::chrono::steady_clock::now();
}

bool StreamParser::isTimeout() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - m_last_activity).count();
    return elapsed >= static_cast<int64_t>(m_timeout_ms);
}

std::string StreamParser::toHex(uint8_t byte) {
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    return oss.str();
}

}  // namespace sp
