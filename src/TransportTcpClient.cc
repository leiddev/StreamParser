/*
 * TransportTcpClient.cc - TCP client transport for the Transport facade.
 *
 * Leid 2026.10
 *
 * One link ("host:port") that reconnects on its own; the transport keeps the
 * connect/disconnect handling out of the business object.
 */
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <sp/StreamParserLog.h>
#include <sp/TcpClientParserAdapter.h>
#include <sp/TcpTypes.h>
#include "TransportImpl.h"

namespace sp {
namespace detail {

class TcpClientTransport : public TransportBase {
public:
    TcpClientTransport(TransportConfig cfg, ParserFactory factory, TransportHandler handler)
        : TransportBase(std::move(cfg), std::move(factory), std::move(handler)), retry_(io()) {
        tag_ = config().peer_host + ":" + std::to_string(config().peer_port);
    }

protected:
    bool open() override {
        if (config().peer_host.empty() || config().peer_port == 0) {
            emitError(config().name, -9, "peer_host / peer_port are not configured");
            return false;
        }

        auto parser = makeParser(tag_);

        TcpClientConfig client_cfg;
        client_cfg.tcp.enable_keepalive           = config().keep_alive;
        client_cfg.tcp.keepalive_idle_seconds     = config().keepalive_idle_seconds;
        client_cfg.tcp.keepalive_interval_seconds = config().keepalive_interval_seconds;
        client_cfg.tcp.keepalive_probes           = config().keepalive_probes;
        client_cfg.tcp.recv_buffer_bytes          = config().recv_buffer_bytes;
        client_cfg.tcp.idle_timeout_ms            = config().link_idle_timeout_ms;
        client_.reset(new TcpClientParserAdapter(io(), config().peer_host, config().peer_port,
                                                std::move(parser), client_cfg));

        client_->setConnectCallback([this](bool ok, const asio::error_code& ec,
                                           const std::string& detail) {
            if (ok) {
                ensureLink();
                if (config().log_link_events) {
                    SP_LOG_INFO("transport", config().name << ": connected to " << tag_);
                }
                emitLinkUp(tag_);
                return;
            }
            emitError(tag_, -10,
                      "connect to " + tag_ + " failed: " +
                          (detail.empty() ? ec.message() : detail));
            scheduleReconnect();
        });

        client_->setDisconnectCallback([this](const std::string& closed) {
            /* The link may already be gone (announced by a failed write); only the
               remover reports the disconnect, and the next connect re-creates it. */
            if (removeLink(closed)) {
                emitLinkDown(closed, "disconnected");
            }
            scheduleReconnect();
        });

        ensureLink();

        client_->connect();
        return true;
    }

    void close() override {
        retry_.cancel();
        if (client_) {
            client_->disconnect();
            client_.reset();
        }
    }

private:
    void scheduleReconnect() {
        if (stopping() || config().reconnect_ms == 0 || !client_) {
            return;
        }
        SP_LOG_WARN("transport", config().name << ": reconnecting to " << tag_ << " in "
                                               << config().reconnect_ms << " ms");
        retry_.expires_after(std::chrono::milliseconds(config().reconnect_ms));
        retry_.async_wait([this](const asio::error_code& ec) {
            if (!ec && !stopping() && client_) {
                client_->connect();
            }
        });
    }

    /* The link table entry follows the connection, not the transport: a failed
       write or a closed socket removes it (and reports the link down), and the
       next successful connect puts it back. */
    void ensureLink() {
        if (findLink(tag_)) {
            return;
        }
        addLink(tag_, [this](const Frame& frame, WriteDone done) {
            std::shared_ptr<TcpClientParserAdapter> client = client_;
            if (!client || !client->is_open()) {
                done(false, "not connected");
                return;
            }
            client->async_write(frame, [done](TcpWriteResult result, size_t /*bytes*/,
                                              const asio::error_code& ec,
                                              const std::string&      detail) {
                done(result == TcpWriteResult::Ok, detail.empty() ? ec.message() : detail);
            });
        });
    }

    std::shared_ptr<TcpClientParserAdapter> client_;
    asio::steady_timer                      retry_;
    std::string                             tag_;
};

}  // namespace detail

std::unique_ptr<Transport> MakeTcpClientTransport(TransportConfig  cfg,
                                                  ParserFactory    factory,
                                                  TransportHandler handler) {
    return std::unique_ptr<Transport>(
        new detail::TcpClientTransport(std::move(cfg), std::move(factory), std::move(handler)));
}

}  // namespace sp
