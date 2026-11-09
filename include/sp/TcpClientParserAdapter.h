/*
 * Leidi 2026.7
 *
 * TcpClientParserAdapter: outbound TCP adapter that bridges an
 * asio::ip::tcp::socket to a StreamParser. Mirrors the lifecycle and
 * read/write/timeout logic of TcpSessionParserAdapter, adding a
 * connect-state machine on top.
 */
#pragma once
#ifndef _TCP_CLIENT_PARSER_ADAPTER_H_
#define _TCP_CLIENT_PARSER_ADAPTER_H_

#include <asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <sp/TcpTypes.h>
#include <sp/StreamParser.h>
#include <sp/StreamParserLog.h>

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
// TcpClientParserAdapter
//
// Outbound TCP adapter. Wraps a non-blocking connect attempt, then
// runs the same read-loop / timeout-timer / atomic-write machinery as
// TcpSessionParserAdapter once the connection is up.
//
// State machine:
//   Disconnected -> Connecting (connect())
//   Connecting   -> Connected  (on_connect ok)
//   Connecting   -> Disconnected (on_connect err / timeout)
//   Connected    -> Closing     (read err / EOF / disconnect())
//   Closing      -> Disconnected
//
// Thread model:
//   - All socket operations run on the io_context thread.
//   - connect(), disconnect(), async_write() and build_and_async_write()
//     are safe to call from any thread: they asio::post() to the socket
//     executor. The calling thread never blocks.
//   - feed() / checkTimeout() are not used externally; if a derived
//     transport needs to invoke them, the caller must synchronize.
// -------------------------------------------------
class TcpClientParserAdapter
    : public std::enable_shared_from_this<TcpClientParserAdapter> {
public:
    enum class State {
        Disconnected,
        Connecting,
        Connected,
        Closing
    };

    /**
     * @param io       Asio io_context
     * @param host     Server IP literal (IPv4 or IPv6). Hostname not supported in v1.
     * @param port     Server TCP port
     * @param parser   Pre-constructed parser (ownership transferred,
     *                 depends only on StreamParser base class)
     * @param config   Client adapter configuration
     */
    TcpClientParserAdapter(asio::io_context&             io,
                           std::string                   host,
                           uint16_t                      port,
                           std::unique_ptr<StreamParser> parser,
                           TcpClientConfig               config = {})
        : socket_(io)
        , timer_(io)
        , connect_timer_(io)
        , parser_(std::move(parser))
        , host_(std::move(host))
        , port_(port)
        , config_(config)
    {
        recv_buf_.resize(config_.tcp.recv_buffer_bytes);
    }

    // Disable copy
    TcpClientParserAdapter(const TcpClientParserAdapter&) = delete;
    TcpClientParserAdapter& operator=(const TcpClientParserAdapter&) = delete;

    // Disable move
    TcpClientParserAdapter(TcpClientParserAdapter&&) = delete;
    TcpClientParserAdapter& operator=(TcpClientParserAdapter&&) = delete;

    // -------------------------------------------------
    // 1. Lifecycle
    // -------------------------------------------------

    // Begin an async connect. Idempotent: a no-op unless state_ == Disconnected.
    // May be called from any thread.
    void connect() {
        asio::post(socket_.get_executor(),
            [self = shared_from_this()]() {
                self->do_connect();
            });
    }

    // Gracefully tear the connection down. Idempotent, safe in any state.
    // Safe to call from any thread; the actual work is posted to the io_context.
    void disconnect() noexcept {
        asio::post(socket_.get_executor(),
            [self = shared_from_this()]() {
                self->do_disconnect();
            });
    }

    ~TcpClientParserAdapter() {
        // Mirror TcpSessionParserAdapter: synchronous cleanup in the destructor.
        // do_close_sync() does not depend on the io_context being alive, which
        // is essential because the io_context may already be destroyed by the
        // time the destructor runs (e.g. when the demo global goes out of scope).
        do_close_sync();
    }

    // -------------------------------------------------
    // 2. Callbacks
    // -------------------------------------------------

    void setConnectCallback(ConnectCallback cb) {
        connect_cb_ = std::move(cb);
    }

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
    // Pre-condition: state_ == Connected. When state_ != Connected the
    // callback fires synchronously with WriteResult::Disconnected; no data
    // is queued.
    //
    // Cancellation: disconnect() or adapter destruction causes all pending
    // writes to receive WriteResult::Cancelled (via operation_aborted).
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

    State state() const { return state_; }
    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }
    bool is_open() const { return state_ == State::Connected && socket_.is_open(); }

private:
    // -------------------------------------------------
    // 5. Connect state machine
    // -------------------------------------------------

    // Must run on io_context thread.
    void do_connect() {
        if (state_ != State::Disconnected) {
            return;
        }

        // Resolve host: literal IPv4 / IPv6 only in v1.
        asio::error_code ec;
        asio::ip::address addr = asio::ip::address::from_string(host_, ec);
        if (ec) {
            std::string detail = "invalid host literal: " + host_ + " (" + ec.message() + ")";
            SP_LOG_ERROR("tcp-client", detail);
            if (connect_cb_) connect_cb_(false, ec, detail);
            return;
        }

        // Recreate socket if it was closed by an earlier disconnect().
        if (!socket_.is_open()) {
            socket_ = asio::ip::tcp::socket(socket_.get_executor());
        }

        state_ = State::Connecting;

        // Start connect timeout.
        connect_timer_.expires_after(std::chrono::milliseconds(config_.connect_timeout_ms));
        connect_timer_.async_wait(
            [self = shared_from_this()](const asio::error_code& tec) {
                if (!tec) {
                    // Timeout fired before connect completed -> cancel in-flight connect.
                    self->on_connect_timeout();
                }
            });

        asio::ip::tcp::endpoint endpoint(addr, port_);
        socket_.async_connect(endpoint,
            [self = shared_from_this()](const asio::error_code& cec) {
                self->on_connect(cec);
            });
    }

    // Must run on io_context thread.
    void on_connect(const asio::error_code& ec) {
        // Cancel the (possibly still-pending) connect timeout.
        asio::error_code cancel_ec;
        connect_timer_.cancel(cancel_ec);

        // Always end up back at Disconnected on failure path.
        if (ec) {
            std::string detail = "connect failed: " + ec.message();
            SP_LOG_WARN("tcp-client", detail << " (host " << host_ << ":" << port_ << ")");
            state_ = State::Disconnected;
            // Best-effort socket cleanup so a subsequent connect() works.
            asio::error_code close_ec;
            socket_.close(close_ec);
            if (connect_cb_) connect_cb_(false, ec, detail);
            return;
        }

        // Success: apply TCP_NODELAY + keepalive, then start read/timeout/parser.
        if (config_.no_delay) {
            asio::ip::tcp::no_delay option(true);
            asio::error_code soptec;
            socket_.set_option(option, soptec);
            if (soptec) {
                SP_LOG_DEBUG("tcp-client", "set_option(TCP_NODELAY) failed: "
                                           << soptec.message());
            }
        }

        enable_keepalive();

        if (parser_) {
            parser_->reset();
        }

        state_ = State::Connected;
        start_read();
        start_timeout_timer();

        if (connect_cb_) connect_cb_(true, {}, "connected to " + remote_endpoint_str());
    }

    // Must run on io_context thread.
    void on_connect_timeout() {
        if (state_ != State::Connecting) {
            return;
        }
        // Forcing the socket to close causes any in-flight async_connect to
        // complete with operation_aborted, which routes through on_connect().
        asio::error_code ec;
        socket_.cancel(ec);
        socket_.close(ec);
        // on_connect will run with operation_aborted and reset state_.
    }

    // Must run on io_context thread.
    void do_disconnect() {
        if (state_ == State::Disconnected) {
            return;
        }
        state_ = State::Closing;

        asio::error_code ec;
        connect_timer_.cancel(ec);
        timer_.cancel(ec);

        if (socket_.is_open()) {
            if (config_.tcp.graceful_shutdown) {
                asio::error_code shud_ec;
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both, shud_ec);
            }
            socket_.cancel(ec);
            socket_.close(ec);
        }

        if (parser_) {
            parser_->reset();
        }

        const std::string tag = client_tag();
        state_ = State::Disconnected;
        if (disconnect_cb_) {
            disconnect_cb_(tag);
        }
    }

    // Synchronous teardown for use in the destructor. Does not assume the
    // io_context is alive; safe to call after io_context destruction.
    void do_close_sync() noexcept {
        if (state_ == State::Disconnected) {
            return;
        }
        state_ = State::Closing;

        asio::error_code ec;
        connect_timer_.cancel(ec);
        timer_.cancel(ec);

        if (socket_.is_open()) {
            if (config_.tcp.graceful_shutdown) {
                asio::error_code shud_ec;
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both, shud_ec);
            }
            socket_.cancel(ec);
            socket_.close(ec);
        }

        if (parser_) {
            parser_->reset();
        }

        const std::string tag = client_tag();
        state_ = State::Disconnected;
        if (disconnect_cb_) {
            disconnect_cb_(tag);
        }
    }

    std::string client_tag() const {
        return host_ + ":" + std::to_string(port_);
    }

    std::string remote_endpoint_str() const {
        asio::error_code ec;
        auto ep = socket_.remote_endpoint(ec);
        if (ec) return client_tag();
        return ep.address().to_string() + ":" + std::to_string(ep.port());
    }

    // -------------------------------------------------
    // 6. Keepalive
    // -------------------------------------------------
    void enable_keepalive() {
        if (!config_.tcp.enable_keepalive) return;

        asio::socket_base::keep_alive option(true);
        socket_.set_option(option);

#ifdef __linux__
        int fd = socket_.native_handle();
        int val = config_.tcp.keepalive_idle_seconds;
        ::setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &val, sizeof(val));
        val = config_.tcp.keepalive_interval_seconds;
        ::setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, &val, sizeof(val));
        val = config_.tcp.keepalive_probes;
        ::setsockopt(fd, SOL_TCP, TCP_KEEPCNT, &val, sizeof(val));
#endif

#ifdef _WIN32
        struct tcp_keepalive {
            ULONG onoff;
            ULONG keepalivetime;
            ULONG keepaliveinterval;
        } k = {
            1,
            static_cast<ULONG>(config_.tcp.keepalive_idle_seconds * 1000),
            static_cast<ULONG>(config_.tcp.keepalive_interval_seconds * 1000)
        };
        DWORD bytes = 0;
        ::WSAIoctl(socket_.native_handle(), SIO_KEEPALIVE_VALS,
                   &k, sizeof(k), nullptr, 0, &bytes, nullptr, nullptr);
#endif
    }

    // -------------------------------------------------
    // 7. Read loop
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
            if (state_ == State::Connected) {
                start_read();
            }
            return;
        }

        // operation_aborted: caused by disconnect() or timer cancel -- ignore.
        if (ec == asio::error::operation_aborted) {
            return;
        }

        // eof: remote end closed gracefully -- normal disconnect.
        if (ec == asio::error::eof) {
            handle_disconnect(ec);
            return;
        }

        // Other errors: connection_reset, network_down, etc.
        SP_LOG_WARN("tcp-client", "[" << client_tag() << "] read error: "
                                      << ec.message());
        handle_disconnect(ec);
    }

    // -------------------------------------------------
    // 8. Timeout timer (frame protocol timeout check)
    // -------------------------------------------------
    void start_timeout_timer() {
        timer_.expires_after(std::chrono::milliseconds(config_.tcp.timer_period_ms));
        timer_.async_wait([self = shared_from_this()](const asio::error_code& ec) {
            if (!ec && self->state_ == State::Connected) {
                if (self->parser_) {
                    self->parser_->checkTimeout();
                }
                if (self->check_idle_timeout()) {
                    return;  // the idle check already dropped the connection
                }
                self->start_timeout_timer();
            }
        });
    }

    // See TcpSessionParserAdapter::check_idle_timeout(): a silent server never
    // completes the pending read, so only an explicit no-data timeout notices it.
    // Returns true when the connection was dropped.
    bool check_idle_timeout() {
        if (config_.tcp.idle_timeout_ms == 0) {
            return false;
        }
        const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - last_rx_)
                                 .count();
        if (idle_ms < static_cast<long long>(config_.tcp.idle_timeout_ms)) {
            return false;
        }
        SP_LOG_WARN("tcp-client", "[" << client_tag() << "] no data for " << idle_ms
                                      << " ms, closing idle link");
        handle_disconnect(asio::error_code{});
        return true;
    }

    // -------------------------------------------------
    // 9. Disconnect handling (from Connected side)
    // -------------------------------------------------
    void handle_disconnect(const asio::error_code& /*ec*/) {
        if (state_ != State::Connected) {
            return;
        }
        state_ = State::Closing;

        asio::error_code xec;
        timer_.cancel(xec);

        if (socket_.is_open()) {
            if (config_.tcp.graceful_shutdown) {
                asio::error_code shud_ec;
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both, shud_ec);
            }
            socket_.cancel(xec);
            socket_.close(xec);
        }

        if (parser_) {
            parser_->reset();
        }

        const std::string tag = client_tag();
        state_ = State::Disconnected;
        if (disconnect_cb_) {
            disconnect_cb_(tag);
        }
    }

    // -------------------------------------------------
    // 10. Write helpers
    // -------------------------------------------------
    static TcpWriteResult map_ec(const asio::error_code& ec, std::string& detail) {
        if (!ec) return TcpWriteResult::Ok;
        detail = ec.message();
        if (ec == asio::error::operation_aborted) return TcpWriteResult::Cancelled;
        if (ec == asio::error::not_connected)     return TcpWriteResult::Disconnected;
        if (ec == asio::error::connection_reset)  return TcpWriteResult::Disconnected;
        if (ec == asio::error::eof)               return TcpWriteResult::Disconnected;
        return TcpWriteResult::IOError;
    }

    void do_async_write(std::shared_ptr<const std::vector<uint8_t>> frame,
                        TcpWriteCallback cb) {
        if (state_ != State::Connected || !socket_.is_open()) {
            if (cb) cb(TcpWriteResult::Disconnected, 0, {}, "socket not connected");
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
    asio::ip::tcp::socket     socket_;
    asio::steady_timer        timer_;
    asio::steady_timer        connect_timer_;
    std::unique_ptr<StreamParser> parser_;
    std::string               host_;
    uint16_t                  port_;
    TcpClientConfig           config_;
    ConnectCallback           connect_cb_;
    DisconnectCallback        disconnect_cb_;

    std::vector<uint8_t>      recv_buf_;
    State                     state_ = State::Disconnected;

    std::chrono::steady_clock::time_point last_rx_ = std::chrono::steady_clock::now();
};

}  // namespace sp

#endif  // !_TCP_CLIENT_PARSER_ADAPTER_H_