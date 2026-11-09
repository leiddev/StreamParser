/*
 * Leid 2026.7
 */
#pragma once
#ifndef _SERIAL_PARSER_ADAPTER_H_
#define _SERIAL_PARSER_ADAPTER_H_

#include <asio.hpp>
#include <memory>
#include <array>
#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <functional>
#include <sp/SerialTypes.h>
#include <sp/StreamParser.h>
#include <sp/StreamParserLog.h>

namespace sp {

// -------------------------------------------------
// SerialParserAdapter
//
// Bridges a serial port to a StreamParser.
// Manages: async read loop, timeout timer, atomic async write.
// -------------------------------------------------
class SerialParserAdapter {
public:
    /**
     * @param io          Asio io_context
     * @param port_name   Serial device name ("/dev/ttyUSB0" or "COM3")
     * @param config      Serial port configuration
     * @param parser      Pre-constructed parser (ownership transferred, depends only on base class)
     */
    SerialParserAdapter(asio::io_context& io,
                        const std::string& port_name,
                        const SerialConfig& config,
                        std::unique_ptr<StreamParser> parser)
        : port_(io)
        , timer_(io)
        , parser_(std::move(parser))
        , port_name_(port_name)
        , config_(config)
    {
        do_open();
    }

    // Disable copy
    SerialParserAdapter(const SerialParserAdapter&) = delete;
    SerialParserAdapter& operator=(const SerialParserAdapter&) = delete;

    // Disable move
    SerialParserAdapter(SerialParserAdapter&&) = delete;
    SerialParserAdapter& operator=(SerialParserAdapter&&) = delete;

    // Optional: modify some settings at runtime
    void set_baud_rate(unsigned int baud) {
        port_.set_option(asio::serial_port_base::baud_rate(baud));
    }

    void open() {
        do_open();
    }

    void close() noexcept {
        if (!port_.is_open())
            return;

        closing_ = true;

        asio::error_code ec;
        timer_.cancel(ec);
        port_.cancel(ec);
        port_.close(ec);

        if (parser_) {
            parser_->reset();
        }
    }

    ~SerialParserAdapter() {
        close();
    }

    // -------------------------------------------------
    // 4. Async atomic write
    // -------------------------------------------------
    // Atomically writes an entire protocol frame to the serial port.
    // Uses asio::async_write so the complete frame is sent without interleaving.
    //
    // Thread model: serial_port is not thread-safe; this function may be called
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
        SerialWriteCallback cb = nullptr) {
        asio::post(port_.get_executor(),
            [self = this, frame = std::move(frame), cb = std::move(cb)]() mutable {
                self->do_async_write(std::move(frame), std::move(cb));
            });
    }

    // Encode payload and send asynchronously.
    // Internally calls parser_->buildFrame(payload). If build succeeds (non-empty frame),
    // posts async_write to the io_context executor. If build fails (empty frame), the
    // callback receives SerialWriteResult::InvalidArg and no data is sent.
    void build_and_async_write(const std::vector<uint8_t>& payload, SerialWriteCallback cb = nullptr) {
        if (!parser_) {
            return;
        }
        auto frame = parser_->buildFrame(payload);
        if (!frame.empty()) {
            auto frame_ptr = std::make_shared<std::vector<uint8_t>>(std::move(frame));
            async_write(frame_ptr, std::move(cb));
            return;
        }
        // Empty frame: fire failure callback if provided
        if (cb) {
            cb(SerialWriteResult::InvalidArg, 0, {}, "buildFrame returned empty");
        }
    }

private:
    void do_open() {
        if (port_.is_open()) {
            asio::error_code ec;
            port_.cancel(ec);
            port_.close(ec);
        }

        // port_.open()/set_option() report failures by throwing asio::system_error.
        // Log them here (the constructor rethrows afterwards) so that a failed
        // device open is visible in the host application's log.
        try {
            port_.open(port_name_);
            apply_config(config_);
        } catch (const std::exception& ex) {
            SP_LOG_ERROR("serial", "open " << port_name_ << " failed: " << ex.what());
            throw;
        }
        closing_ = false;

        if (parser_) {
            parser_->reset();
        }

        SP_LOG_INFO("serial", "opened " << port_name_ << " @ " << config_.baud_rate << " baud");

        start_read();
        start_timeout_timer();
    }

    void apply_config(const SerialConfig& cfg) {
        using spb = asio::serial_port_base;   // Alias for brevity

        port_.set_option(spb::baud_rate(cfg.baud_rate));
        port_.set_option(spb::character_size(cfg.character_size));

        // Parity
        switch (cfg.parity) {
            case SerialConfig::Parity::None:
                port_.set_option(spb::parity(spb::parity::none));
                break;
            case SerialConfig::Parity::Odd:
                port_.set_option(spb::parity(spb::parity::odd));
                break;
            case SerialConfig::Parity::Even:
                port_.set_option(spb::parity(spb::parity::even));
                break;
        }

        // Stop bits
        switch (cfg.stop_bits) {
            case SerialConfig::StopBits::One:
                port_.set_option(spb::stop_bits(spb::stop_bits::one));
                break;
            case SerialConfig::StopBits::OnePointFive:
                port_.set_option(spb::stop_bits(spb::stop_bits::onepointfive));
                break;
            case SerialConfig::StopBits::Two:
                port_.set_option(spb::stop_bits(spb::stop_bits::two));
                break;
        }

        // Flow control
        switch (cfg.flow_control) {
            case SerialConfig::FlowControl::None:
                port_.set_option(spb::flow_control(spb::flow_control::none));
                break;
            case SerialConfig::FlowControl::Software:
                port_.set_option(spb::flow_control(spb::flow_control::software));
                break;
            case SerialConfig::FlowControl::Hardware:
                port_.set_option(spb::flow_control(spb::flow_control::hardware));
                break;
        }
    }

    void start_read() {
        port_.async_read_some(asio::buffer(buf_),
            [this](const asio::error_code& ec, size_t bytes) {
                if (!ec) {
                    parser_->feed(buf_.data(), bytes);
                    start_read();
                } else if (ec != asio::error::operation_aborted) {
                    SP_LOG_WARN("serial", "read error on " << port_name_ << ": "
                                                           << ec.message());
                }
            });
    }

    void start_timeout_timer() {
        timer_.expires_after(std::chrono::milliseconds(30));  // Recommended 20~50ms
        timer_.async_wait([this](const asio::error_code& ec) {
            if (!ec && !closing_) {
                parser_->checkTimeout();
                start_timeout_timer();
            }
        });
    }

    // Translate asio::error_code to SerialWriteResult + human-readable detail.
    static SerialWriteResult map_ec(const asio::error_code& ec, std::string& detail) {
        if (!ec) return SerialWriteResult::Ok;
        detail = ec.message();
        if (ec == asio::error::operation_aborted) return SerialWriteResult::Cancelled;
        if (ec == asio::error::bad_descriptor)     return SerialWriteResult::PortClosed;
        return SerialWriteResult::IOError;
    }

    // Core atomic write: must be called from io_context thread.
    void do_async_write(std::shared_ptr<const std::vector<uint8_t>> frame,
                        SerialWriteCallback cb) {
        if (!port_.is_open()) {
            if (cb) cb(SerialWriteResult::PortClosed, 0, {}, "serial port not open");
            return;
        }
        if (!frame || frame->empty()) {
            if (cb) cb(SerialWriteResult::InvalidArg, 0, {}, "empty frame");
            return;
        }

        asio::async_write(port_, asio::buffer(*frame),
            [frame, cb](const asio::error_code& ec, size_t n) {
                std::string detail;
                auto r = map_ec(ec, detail);
                if (cb) cb(r, n, ec, std::move(detail));
            });
    }

    asio::serial_port             port_;
    asio::steady_timer            timer_;
    std::unique_ptr<StreamParser> parser_;   // Only holds base class
    std::array<uint8_t, 512>      buf_;
    bool                          closing_ = false;
    std::string                   port_name_;
    SerialConfig                  config_;
};

}  // namespace sp

#endif  // !_SERIAL_PARSER_ADAPTER_H_
