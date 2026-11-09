/*
 * TransportTcpServer.cc - TCP server transport for the Transport facade.
 *
 * Leid 2026.10
 *
 * Every accepted connection becomes a link tagged "ip:port"; replies are
 * routed by that tag. The session objects are kept here so close() can shut
 * them all down in one place.
 */
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sp/StreamParserLog.h>
#include <sp/TcpParserServer.h>
#include <sp/TcpSessionParserAdapter.h>
#include <sp/TcpTypes.h>
#include "TransportImpl.h"

namespace sp {
namespace detail {

class TcpServerTransport : public TransportBase {
public:
    TcpServerTransport(TransportConfig cfg, ParserFactory factory, TransportHandler handler)
        : TransportBase(std::move(cfg), std::move(factory), std::move(handler)) {}

protected:
    bool open() override {
        TcpConfig tcp;
        tcp.bind_address = config().bind_address;
        tcp.port         = config().port;
        tcp.backlog      = config().backlog;

        const uint16_t port = config().port;
        server_.reset(new TcpServer(io(), tcp,
                                    [this](asio::ip::tcp::socket socket) {
                                        return onAccept(std::move(socket));
                                    }));
        if (!server_->listen()) {
            server_.reset();
            emitError(config().name, -5, "cannot listen on port " + std::to_string(port));
            return false;
        }
        SP_LOG_INFO("transport", config().name << ": listening on " << config().bind_address << ":"
                                               << port);
        return true;
    }

    void close() override {
        if (server_) {
            server_->stop();
            server_.reset();
        }

        std::vector<std::shared_ptr<TcpSessionParserAdapter>> sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions.reserve(sessions_.size());
            for (auto& kv : sessions_) {
                sessions.push_back(kv.second);
            }
            sessions_.clear();
        }
        for (auto& session : sessions) {
            session->close();
        }
    }

private:
    std::shared_ptr<TcpSessionParserAdapter> onAccept(asio::ip::tcp::socket socket) {
        asio::error_code                  ec;
        const asio::ip::tcp::endpoint     endpoint = socket.remote_endpoint(ec);
        const std::string                 tag = ec ? std::string("unknown") : endpointTag(endpoint);

        if (atCapacity()) {
            emitError(tag, -6, "max_links reached, connection rejected");
            asio::error_code ignored;
            socket.close(ignored);
            return nullptr;   // TcpServer tolerates a null adapter
        }

        auto parser  = makeParser(tag);

        // Accepted sockets get their own keepalive settings: config() describes the
        // listener, but keepalive is a per-connection socket option.
        TcpAdapterConfig session_cfg;
        session_cfg.enable_keepalive           = config().keep_alive;
        session_cfg.keepalive_idle_seconds     = config().keepalive_idle_seconds;
        session_cfg.keepalive_interval_seconds = config().keepalive_interval_seconds;
        session_cfg.keepalive_probes           = config().keepalive_probes;
        session_cfg.recv_buffer_bytes          = config().recv_buffer_bytes;
        session_cfg.idle_timeout_ms            = config().link_idle_timeout_ms;

        auto session = std::make_shared<TcpSessionParserAdapter>(io(), std::move(socket), tag,
                                                                std::move(parser), session_cfg);
        session->setDisconnectCallback([this](const std::string& closed) { onSessionClosed(closed); });

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_[tag] = session;
        }

        addLink(tag, [session](const Frame& frame, WriteDone done) {
            session->async_write(frame, [done](TcpWriteResult result, size_t /*bytes*/,
                                               const asio::error_code& ec,
                                               const std::string&      detail) {
                done(result == TcpWriteResult::Ok, detail.empty() ? ec.message() : detail);
            });
        });

        if (config().log_link_events) {
            SP_LOG_INFO("transport", config().name << ": client " << tag << " connected, "
                                                   << linkCount() << " link(s)");
        }
        emitLinkUp(tag);
        return session;
    }

    void onSessionClosed(const std::string& tag) {
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_.erase(tag);
        }
        /* A failed write may already have removed the link and announced it; only
           the call that actually removes the link reports the disconnect. */
        if (!removeLink(tag)) {
            return;
        }
        if (config().log_link_events) {
            SP_LOG_INFO("transport", config().name << ": client " << tag << " disconnected, "
                                                   << linkCount() << " link(s)");
        }
        emitLinkDown(tag, "connection closed");
    }

    std::unique_ptr<TcpServer>                                  server_;
    std::unordered_map<std::string, std::shared_ptr<TcpSessionParserAdapter>> sessions_;
    std::mutex                                                  sessions_mutex_;
};

}  // namespace detail

std::unique_ptr<Transport> MakeTcpServerTransport(TransportConfig  cfg,
                                                  ParserFactory    factory,
                                                  TransportHandler handler) {
    return std::unique_ptr<Transport>(
        new detail::TcpServerTransport(std::move(cfg), std::move(factory), std::move(handler)));
}

}  // namespace sp
