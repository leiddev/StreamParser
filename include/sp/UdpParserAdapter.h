/*
 * Leid 2026.7
 *
 * UdpParserAdapter: UDP adapter that bridges an asio::ip::udp::socket
 * to a StreamParser. Manages: async receive loop, timeout timer,
 * and async send to specified or last-known endpoint.
 *
 * Unlike TCP, UDP is connectionless. This adapter:
 *   - Binds to a local port for receiving
 *   - Tracks the last sender (remote_endpoint_) for reply convenience
 *   - Supports sending to any endpoint via async_send_to()
 */
#pragma once
#ifndef _UDP_PARSER_ADAPTER_H_
#define _UDP_PARSER_ADAPTER_H_

#include <asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <sp/UdpTypes.h>
#include <sp/StreamParser.h>
#include <sp/StreamParserLog.h>

namespace sp {

// -------------------------------------------------
// UdpParserAdapter
//
// UDP adapter bridging asio::ip::udp::socket to StreamParser.
//
// State machine:
//   Closed -> Opening (open())
//   Opening -> Open (bind success)
//   Opening -> Closed (bind failure)
//   Open    -> Closing (close())
//   Closing -> Closed
//
// Thread model:
//   - All socket operations run on the io_context thread.
//   - open(), close(), async_write(), async_send_to(), and build_and_async_write()
//     are safe to call from any thread: they asio::post() to the socket
//     executor. The calling thread never blocks.
// -------------------------------------------------
class UdpParserAdapter
    : public std::enable_shared_from_this<UdpParserAdapter> {
public:
    // State enum - defined first because accessors depend on it
    enum class State {
        Closed,
        Opening,
        Open,
        Closing
    };

    /**
     * @param io       Asio io_context
     * @param port     Local port to bind (0 = any available port)
     * @param parser   Pre-constructed parser (ownership transferred,
     *                 depends only on StreamParser base class)
     * @param config   Adapter configuration
     */
    UdpParserAdapter(asio::io_context&             io,
                     uint16_t                      port,
                     std::unique_ptr<StreamParser> parser,
                     UdpAdapterConfig              config = {})
        : socket_(io)
        , timer_(io)
        , parser_(std::move(parser))
        , local_port_(port)
        , config_(config)
    {
        recv_buf_.resize(config_.recv_buffer_bytes);
    }

    // Disable copy
    UdpParserAdapter(const UdpParserAdapter&) = delete;
    UdpParserAdapter& operator=(const UdpParserAdapter&) = delete;

    // Disable move
    UdpParserAdapter(UdpParserAdapter&&) = delete;
    UdpParserAdapter& operator=(UdpParserAdapter&&) = delete;

    // -------------------------------------------------
    // 1. Lifecycle
    // -------------------------------------------------

    // Bind to local port and start receiving. Idempotent: no-op if already Open.
    // May be called from any thread.
    void open() {
        asio::post(socket_.get_executor(),
            [self = shared_from_this()]() {
                self->do_open();
            });
    }

    // Close the socket. Idempotent, safe in any state.
    // Safe to call from any thread; the actual work is posted to the io_context.
    void close() noexcept {
        asio::post(socket_.get_executor(),
            [self = shared_from_this()]() {
                self->do_close();
            });
    }

    ~UdpParserAdapter() {
        do_close_sync();
    }

    // -------------------------------------------------
    // 2. Callbacks
    // -------------------------------------------------

    void setOpenCallback(UdpOpenCallback cb) {
        open_cb_ = std::move(cb);
    }

    void setCloseCallback(UdpCloseCallback cb) {
        close_cb_ = std::move(cb);
    }

    // -------------------------------------------------
    // 3. Async send to last-known sender
    // -------------------------------------------------
    // Sends a datagram to the last endpoint that sent us data.
    // If no data has been received yet, the callback fires with NotBound.
    //
    // Thread model: safe to call from any thread (asio::post).
    // Buffer lifetime: frame is captured by shared_ptr in the lambda.
    //
    // @param frame  Complete protocol frame (usually built by StreamParser::buildFrame).
    // @param cb     Completion callback; may be nullptr to fire-and-forget.
    void async_write(std::shared_ptr<const std::vector<uint8_t>> frame,
                     UdpWriteCallback cb = nullptr) {
        asio::post(socket_.get_executor(),
            [self = shared_from_this(),
             frame = std::move(frame),
             cb    = std::move(cb)]() mutable {
                self->do_async_write_to(self->remote_endpoint_, std::move(frame), std::move(cb));
            });
    }

    // -------------------------------------------------
    // 4. Async send to specified endpoint
    // -------------------------------------------------
    // Sends a datagram to an arbitrary UDP endpoint.
    //
    // Thread model: safe to call from any thread (asio::post).
    // Buffer lifetime: frame is captured by shared_ptr in the lambda.
    //
    // @param dest   Destination endpoint (IP:port)
    // @param frame  Complete protocol frame
    // @param cb     Completion callback; may be nullptr to fire-and-forget.
    void async_send_to(const asio::ip::udp::endpoint& dest,
                       std::shared_ptr<const std::vector<uint8_t>> frame,
                       UdpWriteCallback cb = nullptr) {
        asio::post(socket_.get_executor(),
            [self = shared_from_this(),
             dest,
             frame = std::move(frame),
             cb    = std::move(cb)]() mutable {
                self->do_async_write_to(dest, std::move(frame), std::move(cb));
            });
    }

    // -------------------------------------------------
    // 5. Build frame and send to last-known sender
    // -------------------------------------------------
    // Encode payload and send asynchronously to remote_endpoint_.
    // Internally calls parser_->buildFrame(payload). If build succeeds (non-empty frame),
    // posts async_write to the io_context executor. If build fails (empty frame), the
    // callback receives a failure result and no data is sent.
    void build_and_async_write(const std::vector<uint8_t>& payload,
                               UdpWriteCallback cb = nullptr) {
        auto frame = parser_->buildFrame(payload);
        if (!frame.empty()) {
            auto frame_ptr = std::make_shared<std::vector<uint8_t>>(std::move(frame));
            async_write(frame_ptr, std::move(cb));
        } else {
            // Empty frame: fire failure callback if provided
            if (cb) {
                cb(UdpWriteResult::InvalidArg, 0, {}, "buildFrame returned empty");
            }
        }
    }

    // -------------------------------------------------
    // 6. Accessors
    // -------------------------------------------------

    State state() const { return state_; }
    uint16_t local_port() const { return local_port_; }
    bool is_open() const { return state_ == State::Open && socket_.is_open(); }

    // Get the last endpoint that sent us data (for reply convenience)
    const asio::ip::udp::endpoint& last_sender() const { return remote_endpoint_; }

private:
    // -------------------------------------------------
    // 7. Internal operations
    // -------------------------------------------------

    void do_open() {
        if (state_ == State::Open) {
            return;
        }

        state_ = State::Opening;

        // Recreate socket if needed
        if (socket_.is_open()) {
            asio::error_code ec;
            socket_.close(ec);
        }

        asio::error_code ec;
        socket_.open(asio::ip::udp::v4(), ec);
        if (ec) {
            std::string detail = "socket open failed: " + ec.message();
            SP_LOG_ERROR("udp", detail);
            state_ = State::Closed;
            if (open_cb_) open_cb_(false, ec, detail);
            return;
        }

        // Set socket options
        if (config_.reuse_address) {
            socket_.set_option(asio::ip::udp::socket::reuse_address(true), ec);
        }

        // Bind to local port
        asio::ip::udp::endpoint local_ep(asio::ip::udp::v4(), local_port_);
        socket_.bind(local_ep, ec);
        if (ec) {
            std::string detail = "bind failed: " + ec.message();
            SP_LOG_ERROR("udp", detail);
            socket_.close(ec);
            state_ = State::Closed;
            if (open_cb_) open_cb_(false, ec, detail);
            return;
        }

        // Get actual bound port if we asked for port 0
        asio::error_code gec;
        auto bound_ep = socket_.local_endpoint(gec);
        uint16_t actual_port = bound_ep.port();
        if (local_port_ == 0 && actual_port != 0) {
            local_port_ = actual_port;
        }

        state_ = State::Open;

        if (parser_) {
            parser_->reset();
        }

        start_receive();
        start_timeout_timer();

        std::string tag = tag_string();
        SP_LOG_INFO("udp", "bound to " << tag);
        if (open_cb_) open_cb_(true, {}, "bound to " + tag);
    }

    void do_close() {
        if (state_ == State::Closed) {
            return;
        }

        state_ = State::Closing;

        asio::error_code ec;
        timer_.cancel(ec);
        socket_.cancel(ec);
        socket_.close(ec);

        if (parser_) {
            parser_->reset();
        }

        state_ = State::Closed;
        const std::string tag = tag_string();
        if (close_cb_) {
            close_cb_(tag);
        }
    }

    // Synchronous teardown for destructor. Does not assume io_context is alive.
    void do_close_sync() noexcept {
        if (state_ == State::Closed) {
            return;
        }

        state_ = State::Closing;

        asio::error_code ec;
        timer_.cancel(ec);
        socket_.cancel(ec);
        socket_.close(ec);

        if (parser_) {
            parser_->reset();
        }

        state_ = State::Closed;
    }

    std::string tag_string() const {
        return "udp:" + std::to_string(local_port_);
    }

    // -------------------------------------------------
    // 8. Receive loop
    // -------------------------------------------------
    void start_receive() {
        socket_.async_receive_from(
            asio::buffer(recv_buf_),
            remote_endpoint_,
            [self = shared_from_this()](const asio::error_code& ec, size_t bytes) {
                self->handle_receive(ec, bytes);
            });
    }

    void handle_receive(const asio::error_code& ec, size_t bytes) {
        if (!ec) {
            // Update remote endpoint for reply convenience
            if (parser_) {
                parser_->feed(recv_buf_.data(), bytes);
            }
            if (state_ == State::Open) {
                start_receive();
            }
            return;
        }

        // operation_aborted: caused by close() or timer cancel -- ignore
        if (ec == asio::error::operation_aborted) {
            return;
        }

        SP_LOG_WARN("udp", "receive error on " << tag_string() << ": " << ec.message());
        // For UDP, we continue receiving after most errors (unlike TCP)
        if (state_ == State::Open) {
            start_receive();
        }
    }

    // -------------------------------------------------
    // 9. Timeout timer (frame protocol timeout check)
    // -------------------------------------------------
    void start_timeout_timer() {
        timer_.expires_after(std::chrono::milliseconds(config_.timer_period_ms));
        timer_.async_wait([self = shared_from_this()](const asio::error_code& ec) {
            if (!ec && self->state_ == State::Open) {
                if (self->parser_) {
                    self->parser_->checkTimeout();
                }
                self->start_timeout_timer();
            }
        });
    }

    // -------------------------------------------------
    // 10. Send helpers
    // -------------------------------------------------
    static UdpWriteResult map_ec(const asio::error_code& ec, std::string& detail) {
        if (!ec) return UdpWriteResult::Ok;
        detail = ec.message();
        if (ec == asio::error::operation_aborted) return UdpWriteResult::Cancelled;
        if (ec == asio::error::not_socket)        return UdpWriteResult::NotBound;
        if (ec == asio::error::shut_down)         return UdpWriteResult::NotBound;
        return UdpWriteResult::SendFailed;
    }

    void do_async_write_to(const asio::ip::udp::endpoint& dest,
                           std::shared_ptr<const std::vector<uint8_t>> frame,
                           UdpWriteCallback cb) {
        if (state_ != State::Open || !socket_.is_open()) {
            if (cb) cb(UdpWriteResult::NotBound, 0, {}, "socket not open");
            return;
        }

        // Check if destination is valid (endpoint not default-constructed and port != 0)
        if (!dest.address().is_unspecified() && dest.port() == 0) {
            if (cb) cb(UdpWriteResult::InvalidArg, 0, {}, "destination port is 0");
            return;
        }

        if (!frame || frame->empty()) {
            if (cb) cb(UdpWriteResult::InvalidArg, 0, {}, "empty frame");
            return;
        }

        // If destination is unspecified, check if we have a remote endpoint
        if (dest.address().is_unspecified()) {
            if (cb) cb(UdpWriteResult::NotBound, 0, {}, "no destination (remote_endpoint_ is empty)");
            return;
        }

        socket_.async_send_to(
            asio::buffer(*frame),
            dest,
            [frame, cb](const asio::error_code& ec, size_t n) {
                std::string detail;
                auto r = map_ec(ec, detail);
                if (cb) cb(r, n, ec, std::move(detail));
            });
    }

    // -------------------------------------------------
    // Members
    // -------------------------------------------------
    asio::ip::udp::socket     socket_;
    asio::steady_timer        timer_;
    std::unique_ptr<StreamParser> parser_;
    uint16_t                  local_port_;
    UdpAdapterConfig          config_;
    UdpOpenCallback           open_cb_;
    UdpCloseCallback          close_cb_;

    std::vector<uint8_t>     recv_buf_;
    asio::ip::udp::endpoint  remote_endpoint_;  // Last sender, for reply convenience
    State                     state_ = State::Closed;
};

}  // namespace sp

#endif  // !_UDP_PARSER_ADAPTER_H_
