/*
 * Leid 2026.7
 */
#pragma once
#ifndef _TCP_PARSER_SERVER_H_
#define _TCP_PARSER_SERVER_H_

#include <asio.hpp>
#include <functional>
#include <memory>
#include <iostream>
#include <sp/TcpTypes.h>
#include <sp/TcpSessionParserAdapter.h>
#include <sp/StreamParserLog.h>

namespace sp {

// TcpServer: TCP acceptor that listens on a port and manages incoming connections.
// Uses a SessionFactory to create per-connection TcpSessionParserAdapter instances.
class TcpServer {
public:
    TcpServer(asio::io_context& io,
              const TcpConfig& config,
              std::function<std::shared_ptr<TcpSessionParserAdapter>(asio::ip::tcp::socket)> factory)
        : io_(io)
        , acceptor_(io)
        , config_(config)
        , factory_(std::move(factory))
    {}

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    // Returns false when the socket could not be bound or put into listen
    // state; the caller can then treat the server as unavailable.
    bool listen() {
        asio::error_code ec;
        asio::ip::address addr = asio::ip::address::from_string(config_.bind_address, ec);
        if (ec) {
            SP_LOG_WARN("tcp-server", "invalid bind address '" << config_.bind_address
                                              << "', falling back to 0.0.0.0: "
                                              << ec.message());
            addr = asio::ip::address_v4::any();
        }
        asio::ip::tcp::endpoint endpoint(addr, config_.port);
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            SP_LOG_ERROR("tcp-server", "open error on " << config_.bind_address << ":"
                                                       << config_.port << ": " << ec.message());
            return false;
        }
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        if (ec) {
            SP_LOG_ERROR("tcp-server", "bind error on " << config_.bind_address << ":"
                                                       << config_.port << ": " << ec.message());
            return false;
        }
        acceptor_.listen(config_.backlog, ec);
        if (ec) {
            SP_LOG_ERROR("tcp-server", "listen error: " << ec.message());
            return false;
        }
        SP_LOG_INFO("tcp-server", "listening on " << config_.bind_address
                                                  << ":" << config_.port);
        do_accept();
        return true;
    }

    void stop() noexcept {
        if (!acceptor_.is_open()) return;
        asio::error_code ec;
        acceptor_.cancel(ec);
        acceptor_.close(ec);
        stop_log_guard();
    }

    ~TcpServer() {
        stop();
    }

private:
    // stop() is noexcept, so the informational record is emitted defensively.
    void stop_log_guard() noexcept {
        try {
            SP_LOG_INFO("tcp-server", "stopped");
        } catch (...) {
        }
    }

    void do_accept() {
        acceptor_.async_accept(
            [this](const asio::error_code& ec, asio::ip::tcp::socket socket) {
                if (ec) {
                    if (ec != asio::error::operation_aborted) {
                        SP_LOG_ERROR("tcp-server", "accept error: " << ec.message());
                        // Keep accepting: a single failed connection attempt
                        // (e.g. a reset from the peer) must not kill the server.
                        do_accept();
                    }
                    return;
                }
                auto adapter = factory_(std::move(socket));
                if (!adapter) {
                    SP_LOG_WARN("tcp-server", "session factory returned no adapter, "
                                              "connection dropped");
                } else {
                    adapter->start();
                }
                do_accept();
            });
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    TcpConfig config_;
    std::function<std::shared_ptr<TcpSessionParserAdapter>(asio::ip::tcp::socket)> factory_;
};

}  // namespace sp

#endif  // !_TCP_PARSER_SERVER_H_
