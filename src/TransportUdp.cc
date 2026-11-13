/*
 * TransportUdp.cc - UDP transport for the Transport facade.
 *
 * Leid 2026.10
 *
 * UDP has no connections, so a "link" is a remembered peer: the first datagram
 * from an address creates one, and replies are routed back to it by tag.
 * send_to() additionally allows an explicit destination, either numeric or a
 * host name (resolved asynchronously).
 */
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <sp/StreamParserLog.h>
#include "TransportImpl.h"
#include <sp/UdpParserAdapter.h>
#include <sp/UdpTypes.h>

namespace sp {
namespace detail {

class UdpTransport : public TransportBase {
public:
    UdpTransport(TransportConfig cfg, ParserFactory factory, TransportHandler handler)
        : TransportBase(std::move(cfg), std::move(factory), std::move(handler)) {
        local_tag_ = "udp:" + std::to_string(config().port);
    }

protected:
    bool open() override {
        UdpAdapterConfig udp_config;
        udp_config.recv_buffer_bytes = config().recv_buffer_bytes;

        /* The parser is shared by all peers (a datagram carries its peer
           address), so the tag is resolved from the sender of the datagram
           that is currently being parsed. */
        auto parser = createParser(local_tag_);
        wireParser(parser.get(), [this]() { return peerTag(adapter_->last_sender()); });

        adapter_.reset(new UdpParserAdapter(io(), config().port, std::move(parser), udp_config));
        adapter_->setOpenCallback([this](bool ok, const asio::error_code& ec,
                                         const std::string& detail) {
            if (!ok) {
                emitError(config().name, -8,
                          "cannot bind udp port " + std::to_string(config().port) + ": " +
                              (detail.empty() ? ec.message() : detail));
                return;
            }
            /* port 0 means "let the OS pick": the tag can only carry the real
               port once bind() has returned, not before it. */
            local_tag_ = "udp:" + std::to_string(adapter_->local_port());
            if (config().log_link_events) {
                SP_LOG_INFO("transport", config().name << ": udp socket bound on port "
                                                       << adapter_->local_port());
            }
            emitLinkUp(local_tag_);
            if (!config().peer_host.empty() && config().peer_port != 0) {
                resolveAndRegister(config().peer_host, config().peer_port);
            }
        });
        adapter_->setCloseCallback([this](const std::string& tag) {
            if (removeLink(tag)) {
                emitLinkDown(tag, "socket closed");
            }
        });

        adapter_->open();
        return true;   // the bind result is reported through on_error / on_link_up
    }

    void close() override {
        if (adapter_) {
            adapter_->close();
            adapter_.reset();
        }
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers_.clear();
    }

    bool doSendTo(const std::string& host, uint16_t port, const Frame& frame) override {
        if (!adapter_) {
            return false;
        }
        asio::error_code ec;
        const asio::ip::address address = asio::ip::make_address(host, ec);
        if (!ec) {
            sendFrameTo(asio::ip::udp::endpoint(address, port), frame);
            return true;
        }
        resolveAndRegister(host, port, frame);
        return true;
    }

private:
    /* --- peer bookkeeping ------------------------------------------------ */
    std::string peerTag(const asio::ip::udp::endpoint& peer) {
        const std::string tag = endpointTag(peer);
        bool is_new = false;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            is_new = peers_.emplace(tag, peer).second;
        }
        if (!is_new) {
            return tag;
        }
        if (atCapacity()) {
            emitError(tag, -6, "max_links reached, peer ignored");
            std::lock_guard<std::mutex> lock(peers_mutex_);
            peers_.erase(tag);
            return tag;
        }
        /* The destination is resolved from `tag` on every write, so the endpoint
           itself is not needed here. */
        registerPeerLink(tag);
        return tag;
    }

    void registerPeerLink(const std::string& tag) {
        addLink(tag, [this, tag](const Frame& frame, WriteDone done) {
            std::shared_ptr<UdpParserAdapter> adapter = adapter_;
            if (!adapter) {
                done(false, "udp socket is closed");
                return;
            }
            asio::ip::udp::endpoint dest;
            bool                       known = false;
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = peers_.find(tag);
                if (it != peers_.end()) {
                    dest  = it->second;
                    known = true;
                }
            }
            if (!known) {
                /* Report outside the lock: `done` runs the host's error and
                   link-down callbacks, and nests link_mutex_ on the way. */
                done(false, "unknown udp peer " + tag);
                return;
            }
            adapter->async_send_to(dest, frame, [done](UdpWriteResult result, size_t /*bytes*/,
                                                       const asio::error_code& ec,
                                                       const std::string&      detail) {
                done(result == UdpWriteResult::Ok, detail.empty() ? ec.message() : detail);
            });
        });
        if (config().log_link_events) {
            SP_LOG_INFO("transport", config().name << ": udp peer " << tag << " registered");
        }
        emitLinkUp(tag);
    }

    void sendFrameTo(const asio::ip::udp::endpoint& dest, const Frame& frame) {
        const std::string tag = endpointTag(dest);
        adapter_->async_send_to(dest, frame, [this, tag, frame](UdpWriteResult result,
                                                                size_t /*bytes*/,
                                                                const asio::error_code& ec,
                                                                const std::string&      detail) {
            if (result != UdpWriteResult::Ok) {
                emitError(tag, 1, detail.empty() ? ec.message() : detail);
                return;
            }
            emitSent(tag, frame);
        });
    }

    /* Resolve `host` and, when `frame` is empty, only register the peer link;
       otherwise resolve and then send that one frame. */
    void resolveAndRegister(const std::string& host, uint16_t port, const Frame& frame = nullptr) {
        auto resolver = std::make_shared<asio::ip::udp::resolver>(io());
        resolver->async_resolve(host, std::to_string(port),
                                [this, resolver, frame, host](const asio::error_code& ec,
                                                        const asio::ip::udp::resolver::results_type&
                                                            results) {
            if (ec || results.empty()) {
                emitError(config().name, -7,
                          "cannot resolve " + std::string("udp peer: ") +
                              (ec ? ec.message() : std::string("no result")));
                return;
            }
            /* The socket is IPv4, so an IPv6 answer ("localhost" resolves to
               ::1 first on many hosts) cannot be used: take the first IPv4
               address instead of failing on a v6 endpoint. */
            asio::ip::udp::endpoint peer;
            bool                    found = false;
            for (auto it = results.begin(); it != results.end(); ++it) {
                if (it->endpoint().address().is_v4()) {
                    peer  = it->endpoint();
                    found = true;
                    break;
                }
            }
            if (!found) {
                emitError(config().name, -7, "cannot resolve udp peer: no IPv4 address for " + host);
                return;
            }
            const std::string             tag  = peerTag(peer);
            if (frame) {
                pushAndFlush(findLink(tag), frame);
            }
        });
    }

    std::shared_ptr<UdpParserAdapter>                            adapter_;
    std::string                                                  local_tag_;
    std::unordered_map<std::string, asio::ip::udp::endpoint>     peers_;
    std::mutex                                                   peers_mutex_;
};

}  // namespace detail

std::unique_ptr<Transport> MakeUdpTransport(TransportConfig  cfg,
                                            ParserFactory    factory,
                                            TransportHandler handler) {
    return std::unique_ptr<Transport>(
        new detail::UdpTransport(std::move(cfg), std::move(factory), std::move(handler)));
}

}  // namespace sp
