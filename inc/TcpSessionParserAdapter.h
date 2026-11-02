/*
 * Leid 2026.7
 */
#pragma once
#ifndef _TCP_SESSION_PARSER_ADAPTER_H_
#define _TCP_SESSION_PARSER_ADAPTER_H_

#include <asio.hpp>
#include <memory>
#include <array>
#include <chrono>
#include <string>
#include <vector>
#include <functional>
#include "TcpTypes.h"
#include "StreamParser.h"
#include "StreamParserLog.h"

#ifdef __linux__
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#ifdef _WIN32
// winsock2.h must come before windows.h, otherwise the legacy winsock.h is
// pulled in first and the two sets of socket definitions conflict.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <mstcpip.h>
#endif

namespace sp {

// -------------------------------------------------
// TcpSessionParserAdapter
//
// Per-connection adapter that bridges a TCP socket to a StreamParser.
// Manages: async read loop, timeout timer, atomic async write,
// graceful shutdown, and disconnect notification.
//
// Thread model:
//   - All socket operations must run on the io_context thread.
//   - async_write() is safe to call from any thread: it uses
//     asio::post() to forward to the io_context executor.
//   - feed() and checkTimeout() are called internally; if called
//     from external threads, the caller must synchronize or post.
// -------------------------------------------------
class TcpSessionParserAdapter
    : public std::enable_shared_from_this<TcpSessionParserAdapter> {
public:
    /**
     * @param io       Asio io_context
     * @param socket   Accepted TCP socket (ownership transferred)
     * @param tag      Connection identifier (e.g. "ip:port")
     * @param parser   Pre-constructed parser (ownership transferred,
     *                 depends only on StreamParser base class)
     * @param config   TCP adapter configuration
     */
    TcpSessionParserAdapter(asio::io_context&                io,
                            asio::ip::tcp::socket             socket,
                            const std::string&               tag,
                            std::unique_ptr<StreamParser>     parser,
                            TcpAdapterConfig                 config = {})
        : socket_(std::move(socket))
        , timer_(io)
        , parser_(std::move(parser))
        , tag_(tag)
        , config_(config)
    {}

    // Disable copy
    TcpSessionParserAdapter(const TcpSessionParserAdapter&) = delete;
    TcpSessionParserAdapter& operator=(const TcpSessionParserAdapter&) = delete;

    // Disable move
    TcpSessionParserAdapter(TcpSessionParserAdapter&&) = delete;
    TcpSessionParserAdapter& operator=(TcpSessionParserAdapter&&) = delete;

    // -------------------------------------------------
    // 1. Lifecycle
    // -------------------------------------------------

    // Start the read loop and timeout timer.
    // Must be called after construction, from the io_context thread.
    void start() {
        reset_and_start();
    }

    // Gracefully or forcibly close the connection.
    // Idempotent: safe to call multiple times.
    void close() noexcept {
        if (closing_) return;
        closing_ = true;

        asio::error_code ec;
        timer_.cancel(ec);

        if (config_.graceful_shutdown && socket_.is_open()) {
            asio::error_code shud_ec;
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, shud_ec);
        }

        socket_.cancel(ec);
        socket_.close(ec);

        if (parser_) {
            parser_->reset();
        }
    }

    ~TcpSessionParserAdapter() {
        close();
    }

    // -------------------------------------------------
    // 2. Callbacks
    // -------------------------------------------------

    void setDisconnectCallback(DisconnectCallback cb) {
        disconnect_cb_ = std::move(cb);
    }

    // -------------------------------------------------
    // 3. Async atomic write
    // -------------------------------------------------
    // Atomically writes an entire protocol frame to the socket.
    // Uses asio::async_write so the complete frame is sent without interleaving.
    //
    // Thread model: socket is not thread-safe; this function may be called
    // from any thread. Internally asio::post() forwards execution to the
    // io_context thread, so the calling thread never blocks.
    //
    // Buffer lifetime: frame is captured by shared_ptr in the lambda, so the
    // buffer stays alive until the callback fires. The caller only needs to
    // ensure frame is valid at call time.
    //
    // Atomic guarantee: asio::async_write sends the entire buffer before any
    // other write operation can interleave. Partial failure is not possible;
    // bytes is either frame->size() on success or 0 on failure.
    //
    // Cancellation: close() or adapter destruction causes all pending writes
    // to receive WriteResult::Cancelled.
    //
    // @param frame  Complete protocol frame (usually built by StreamParser::buildFrame).
    //               Must be non-null and non-empty.
    // @param cb     Completion callback; may be nullptr to fire-and-forget.
    void async_write(std::shared_ptr<const std::vector<uint8_t>> frame,
                     TcpWriteCallback cb = nullptr) {
        asio::post(socket_.get_executor(),
            [self = shared_from_this(),
             frame = std::move(frame),
             cb    = std::move(cb)]() mutable {
                self->do_async_write(std::move(frame), std::move(cb));
            });
    }

    // Encode payload and send asynchronously.
    // Internally calls parser_->buildFrame(payload). If build succeeds (non-empty frame),
    // posts async_write to the io_context executor. If build fails (empty frame), the
    // callback receives a failure result and no data is sent.
    void build_and_async_write(const std::vector<uint8_t>& payload,
                               TcpWriteCallback cb = nullptr) {
        if (parser_) {
            auto frame = parser_->buildFrame(payload);
            if (!frame.empty()) {
                auto frame_ptr = std::make_shared<std::vector<uint8_t>>(std::move(frame));
                async_write(frame_ptr, std::move(cb));
            } else {
                // Empty frame: fire failure callback if provided
                if (cb) {
                    cb(TcpWriteResult::InvalidArg, 0, {}, "buildFrame returned empty");
                }
            }
        }
    }

    // -------------------------------------------------
    // 4. Accessors
    // -------------------------------------------------

    const std::string& tag() const { return tag_; }
    bool is_open() const { return socket_.is_open() && !closing_; }

private:
    void reset_and_start() {
        closing_ = false;
        if (parser_) {
            parser_->reset();
        }
        enable_keepalive();
        start_read();
        start_timeout_timer();
    }

    // -------------------------------------------------
    // 5. Keepalive
    // -------------------------------------------------
    void enable_keepalive() {
        if (!config_.enable_keepalive) return;

        asio::socket_base::keep_alive option(true);
        socket_.set_option(option);

#ifdef __linux__
        int fd = socket_.native_handle();
        int val = config_.keepalive_idle_seconds;
        ::setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &val, sizeof(val));
        val = config_.keepalive_interval_seconds;
        ::setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, &val, sizeof(val));
        val = config_.keepalive_probes;
        ::setsockopt(fd, SOL_TCP, TCP_KEEPCNT, &val, sizeof(val));
#endif

#ifdef _WIN32
        tcp_keepalive k;
        k.onoff = 1;
        k.keepalivetime = static_cast<ULONG>(config_.keepalive_idle_seconds * 1000);
        k.keepaliveinterval = static_cast<ULONG>(config_.keepalive_interval_seconds * 1000);
        DWORD bytes = 0;
        ::WSAIoctl(socket_.native_handle(), SIO_KEEPALIVE_VALS,
                   &k, sizeof(k), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    }

    // -------------------------------------------------
    // 5. Read loop
    // -------------------------------------------------
    void start_read() {
        last_rx_ = std::chrono::steady_clock::now();
        socket_.async_read_some(
            asio::buffer(recv_buf_),
            [self = shared_from_this()](const asio::error_code& ec, size_t bytes) {
                self->handle_read(ec, bytes);
            });
    }

    void handle_read(const asio::error_code& ec, size_t bytes) {
        if (!ec) {
            if (parser_) {
                parser_->feed(recv_buf_.data(), bytes);
            }
            start_read();
            return;
        }

        // operation_aborted: caused by close() or timer cancel -- ignore
        if (ec == asio::error::operation_aborted) {
            return;
        }

        // eof: remote end closed gracefully -- normal disconnect
        if (ec == asio::error::eof) {
            handle_disconnect();
            return;
        }

        // Other errors: connection_reset, network_down, etc.
        SP_LOG_WARN("tcp", "[" << tag_ << "] TCP read error: " << ec.message());
        handle_disconnect();
    }

    // -------------------------------------------------
    // 6. Timeout timer
    // -------------------------------------------------
    void start_timeout_timer() {
        timer_.expires_after(std::chrono::milliseconds(config_.timer_period_ms));
        timer_.async_wait([self = shared_from_this()](const asio::error_code& ec) {
            if (!ec && !self->closing_) {
                if (self->parser_) {
                    self->parser_->checkTimeout();
                }
                if (self->check_idle_timeout()) {
                    return;  // the idle check already closed the session
                }
                self->start_timeout_timer();
            }
        });
    }

    // A peer that went silent (power loss behind a half-open proxy, hung app)
    // never completes the pending read, so the frame timeout above cannot see
    // it: that one only fires while a frame is half received. Close the session
    // when not a single byte arrived for idle_timeout_ms.
    // Returns true when the session was closed.
    bool check_idle_timeout() {
        if (config_.idle_timeout_ms == 0) {
            return false;
        }
        const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - last_rx_)
                                 .count();
        if (idle_ms < static_cast<long long>(config_.idle_timeout_ms)) {
            return false;
        }
        SP_LOG_WARN("tcp", "[" << tag_ << "] no data for " << idle_ms
                               << " ms, closing idle link");
        handle_disconnect();
        return true;
    }

    // -------------------------------------------------
    // 7. Disconnect handling
    // -------------------------------------------------
    void handle_disconnect() {
        if (closing_) return;

        closing_ = true;

        asio::error_code ec;
        timer_.cancel(ec);
        socket_.cancel(ec);
        socket_.close(ec);

        if (parser_) {
            parser_->reset();
        }

        if (disconnect_cb_) {
            disconnect_cb_(tag_);
        }
    }

    // -------------------------------------------------
    // 8. Write helpers
    // -------------------------------------------------
    static TcpWriteResult map_ec(const asio::error_code& ec, std::string& detail) {
        if (!ec) return TcpWriteResult::Ok;
        detail = ec.message();
        if (ec == asio::error::operation_aborted) return TcpWriteResult::Cancelled;
        if (ec == asio::error::not_connected)     return TcpWriteResult::Disconnected;
        if (ec == asio::error::connection_reset)   return TcpWriteResult::Disconnected;
        if (ec == asio::error::eof)                return TcpWriteResult::Disconnected;
        return TcpWriteResult::IOError;
    }

    void do_async_write(std::shared_ptr<const std::vector<uint8_t>> frame,
                        TcpWriteCallback cb) {
        if (!socket_.is_open()) {
            if (cb) cb(TcpWriteResult::Disconnected, 0, {}, "socket not open");
            return;
        }
        if (!frame || frame->empty()) {
            if (cb) cb(TcpWriteResult::InvalidArg, 0, {}, "empty frame");
            return;
        }

        asio::async_write(socket_, asio::buffer(*frame),
            [frame, cb](const asio::error_code& ec, size_t n) {
                std::string detail;
                auto r = map_ec(ec, detail);
                if (cb) cb(r, n, ec, std::move(detail));
            });
    }

    // -------------------------------------------------
    // Members
    // -------------------------------------------------
    asio::ip::tcp::socket              socket_;
    asio::steady_timer             timer_;
    std::unique_ptr<StreamParser> parser_;
    std::string                    tag_;
    TcpAdapterConfig                config_;
    bool                           closing_ = false;
    DisconnectCallback             disconnect_cb_;

    std::chrono::steady_clock::time_point last_rx_ = std::chrono::steady_clock::now();
    std::array<uint8_t, 4096>      recv_buf_;
};

}  // namespace sp

#endif  // !_TCP_SESSION_PARSER_ADAPTER_H_
