/*
 * TransportTestKit.h - helpers shared by the Transport facade tests.
 *
 * The facade's callbacks all run on its io thread, so a test has to record
 * them under a lock and then wait for them from the test thread. Everything
 * here is header only and test-only: it is never installed.
 *
 * The checks themselves stay in the per-file `check()` style the other tests
 * use, so there is still no test framework to fetch.
 */
#pragma once
#ifndef _SP_TRANSPORT_TEST_KIT_H_
#define _SP_TRANSPORT_TEST_KIT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sp/StreamParser.h>
#include <sp/Transport.h>

namespace sptest {

// How long a wait_for() call is allowed to take. Reconnect, idle timeout and
// frame timeout tests all run against real clocks, so be generous.
constexpr int kWaitMs = 3000;

inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// -------------------------------------------------
// Recorded events
// -------------------------------------------------
struct Frame {
    std::string          tag;
    std::vector<uint8_t> payload;
};

struct LinkEvent {
    std::string tag;
    std::string why;
};

struct ErrorEvent {
    std::string tag;
    int         code = 0;
    std::string detail;
};

// Records every callback the facade can deliver. The facade calls them on its
// own io thread, so every read goes through the lock.
class Recorder {
public:
    sp::TransportHandler handler() {
        sp::TransportHandler h;
        h.on_frame = [this](const std::string& tag, const uint8_t* data, std::size_t len) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                frames_.push_back(Frame{tag, std::vector<uint8_t>(data, data + len)});
                frames_seen_.store(frames_.size());
            }
            cv_.notify_all();
        };
        h.on_link_up = [this](const std::string& tag) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ups_.push_back(tag);
                link_ups_seen_.store(ups_.size());
            }
            cv_.notify_all();
        };
        h.on_link_down = [this](const std::string& tag, const std::string& why) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                downs_.push_back(LinkEvent{tag, why});
                link_downs_seen_.store(downs_.size());
            }
            cv_.notify_all();
        };
        h.on_error = [this](const std::string& tag, int code, const std::string& detail) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                errors_.push_back(ErrorEvent{tag, code, detail});
                errors_seen_.store(errors_.size());
            }
            cv_.notify_all();
        };
        h.on_sent = [this](const std::string& tag, const std::vector<uint8_t>& frame) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sent_.push_back(Frame{tag, frame});
                sent_seen_.store(sent_.size());
            }
            cv_.notify_all();
        };
        return h;
    }

    // Waits until at least `n` of this event kind have been recorded. These
    // counts are atomic on purpose: a predicate handed to wait_for() must not
    // go through the locking accessors below, which would take the same lock
    // the wait holds.
    std::size_t frames_seen() const { return frames_seen_.load(); }
    std::size_t link_ups_seen() const { return link_ups_seen_.load(); }
    std::size_t link_downs_seen() const { return link_downs_seen_.load(); }
    std::size_t errors_seen() const { return errors_seen_.load(); }
    std::size_t sent_seen() const { return sent_seen_.load(); }

    bool wait_frames(std::size_t n, int timeout_ms = kWaitMs) {
        return wait_until([this, n]() { return frames_seen_.load() >= n; }, timeout_ms);
    }
    bool wait_link_ups(std::size_t n, int timeout_ms = kWaitMs) {
        return wait_until([this, n]() { return link_ups_seen_.load() >= n; }, timeout_ms);
    }
    bool wait_link_downs(std::size_t n, int timeout_ms = kWaitMs) {
        return wait_until([this, n]() { return link_downs_seen_.load() >= n; }, timeout_ms);
    }
    bool wait_errors(std::size_t n, int timeout_ms = kWaitMs) {
        return wait_until([this, n]() { return errors_seen_.load() >= n; }, timeout_ms);
    }
    bool wait_sent(std::size_t n, int timeout_ms = kWaitMs) {
        return wait_until([this, n]() { return sent_seen_.load() >= n; }, timeout_ms);
    }

    // Waits for an error with a given code. The list is small enough to search
    // under the wait's own lock.
    bool wait_error_code(int code, int timeout_ms = kWaitMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mutex_);
        while (!has_error_code(code)) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return has_error_code(code);
            }
        }
        return true;
    }

    // Waits for a predicate that must not touch this recorder's locking
    // accessors (use the atomic counters above inside it).
    bool wait_until(const std::function<bool()>& pred, int timeout_ms = kWaitMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mutex_);
        while (!pred()) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return pred();
            }
        }
        return true;
    }

    // Locking snapshots, for assertions rather than for waiting.
    std::size_t frame_count() { return frames_seen(); }
    std::size_t link_up_count() { return link_ups_seen(); }
    std::size_t link_down_count() { return link_downs_seen(); }
    std::size_t error_count() { return errors_seen(); }
    std::size_t sent_count() { return sent_seen(); }

    std::vector<Frame> frames() {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    std::vector<std::string> link_ups() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ups_;
    }
    std::vector<LinkEvent> link_downs() {
        std::lock_guard<std::mutex> lock(mutex_);
        return downs_;
    }
    std::vector<ErrorEvent> errors() {
        std::lock_guard<std::mutex> lock(mutex_);
        return errors_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        ups_.clear();
        downs_.clear();
        errors_.clear();
        sent_.clear();
        frames_seen_.store(0);
        link_ups_seen_.store(0);
        link_downs_seen_.store(0);
        errors_seen_.store(0);
        sent_seen_.store(0);
    }

private:
    // Runs with mutex_ held, so it must not take it again.
    bool has_error_code(int code) {
        for (const ErrorEvent& e : errors_) {
            if (e.code == code) {
                return true;
            }
        }
        return false;
    }

    std::mutex              mutex_;
    std::condition_variable cv_;
    std::vector<Frame>           frames_;
    std::vector<std::string>     ups_;
    std::vector<LinkEvent>       downs_;
    std::vector<ErrorEvent>      errors_;
    std::vector<Frame>           sent_;
    // Mirrors of the sizes above, readable while the wait holds the lock.
    std::atomic<std::size_t>     frames_seen_{0};
    std::atomic<std::size_t>     link_ups_seen_{0};
    std::atomic<std::size_t>     link_downs_seen_{0};
    std::atomic<std::size_t>     errors_seen_{0};
    std::atomic<std::size_t>     sent_seen_{0};
};

// -------------------------------------------------
// A parser for the facade tests
// -------------------------------------------------
// 0x7E | len big endian (2 bytes) | payload | xor of the length bytes and the
// payload. The length field counts the payload only.
//
// The example A55A55AAParser would do for the happy paths, but these tests
// also need a parser that refuses to encode on demand ("the host's protocol
// said no"), so the framing lives here and stays under the test's control.
class FakeParser : public sp::StreamParser {
public:
    FakeParser(const std::string& tag, uint32_t timeout_ms, std::size_t max_frame_size)
        : sp::StreamParser(tag, timeout_ms, max_frame_size) {}

    static std::vector<uint8_t> frame(const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> out;
        out.reserve(payload.size() + 5);
        out.push_back(0x7E);
        out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
        uint8_t x = static_cast<uint8_t>((payload.size() >> 8) & 0xFF) ^
                    static_cast<uint8_t>(payload.size() & 0xFF);
        for (uint8_t b : payload) {
            out.push_back(b);
            x ^= b;
        }
        out.push_back(x);
        return out;
    }

    // When set, this parser refuses to encode and the transport has to report
    // the refusal.
    void refuse_encoding(bool refuse) { refuse_encoding_ = refuse; }

    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) override {
        if (refuse_encoding_) {
            return {};
        }
        std::vector<uint8_t> out = frame(payload);
        // A payload that would not fit max_frame_bytes is refused silently:
        // the budget is the transport's own, and it is what reports it.
        if (out.size() > m_max_frame_size) {
            return {};
        }
        return out;
    }

protected:
    void processByte(uint8_t byte) override {
        switch (m_state) {
            case 0:
                if (byte == 0x7E) {
                    m_buf.assign(1, byte);
                    m_state = 1;
                }
                break;
            case 1:
                m_buf.push_back(byte);
                m_state = 2;
                break;
            case 2: {
                m_buf.push_back(byte);
                const std::size_t len = (static_cast<std::size_t>(m_buf[1]) << 8) | m_buf[2];
                m_expected_len        = len;
                m_state               = 3;
                break;
            }
            case 3:
                m_buf.push_back(byte);
                if (m_buf.size() == 3 + m_expected_len + 1) {
                    uint8_t x = static_cast<uint8_t>(m_buf[1] ^ m_buf[2]);
                    for (std::size_t i = 0; i < m_expected_len; ++i) {
                        x ^= m_buf[3 + i];
                    }
                    if (x != m_buf.back()) {
                        reportError(ErrorCode::ChecksumFail, "xor mismatch");
                        reset();
                        break;
                    }
                    completeFrame(3, m_expected_len);
                }
                break;
            default:
                reset();
                break;
        }
    }

private:
    bool refuse_encoding_ = false;
};

// A factory that makes FakeParser instances. With refuse_from_encoding the
// parsers refuse to encode, which is how a test makes the transport report an
// encoding failure the framing itself did not cause.
inline sp::ParserFactory fake_factory(uint32_t    timeout_ms          = 300,
                                      std::size_t max_frame           = 10 * 1024,
                                      bool        refuse_from_encoding = false) {
    return [timeout_ms, max_frame, refuse_from_encoding](const std::string& tag) {
        std::unique_ptr<FakeParser> parser(new FakeParser(tag, timeout_ms, max_frame));
        parser->refuse_encoding(refuse_from_encoding);
        return std::unique_ptr<sp::StreamParser>(std::move(parser));
    };
}

// -------------------------------------------------
// Small conveniences
// -------------------------------------------------
inline std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

inline std::string as_string(const std::vector<uint8_t>& v) {
    return std::string(v.begin(), v.end());
}

}  // namespace sptest

#endif  // !_SP_TRANSPORT_TEST_KIT_H_
