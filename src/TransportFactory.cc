/*
 * TransportFactory.cc - Transport facade entry point.
 *
 * Leid 2026.10
 */
#include <memory>
#include <string>
#include <utility>

#include "StreamParserLog.h"
#include "TransportImpl.h"

namespace sp {

std::unique_ptr<Transport> CreateTransport(const TransportConfig& cfg,
                                           ParserFactory          parser_factory,
                                           TransportHandler       handler) {
    /* The protocol belongs to the caller: without a factory there is no parser
       and no encoder, so fail here instead of building a transport that can
       never carry a frame. */
    if (!parser_factory) {
        SP_LOG_ERROR("transport", "CreateTransport(" << cfg.name
                                                     << "): parser factory is required");
        return nullptr;
    }

    switch (cfg.kind) {
        case TransportKind::Serial:
            return MakeSerialTransport(cfg, std::move(parser_factory), std::move(handler));
        case TransportKind::TcpServer:
            return MakeTcpServerTransport(cfg, std::move(parser_factory), std::move(handler));
        case TransportKind::TcpClient:
            return MakeTcpClientTransport(cfg, std::move(parser_factory), std::move(handler));
        case TransportKind::Udp:
            return MakeUdpTransport(cfg, std::move(parser_factory), std::move(handler));
    }
    SP_LOG_ERROR("transport", "unknown TransportKind " << static_cast<int>(cfg.kind));
    return nullptr;
}

}  // namespace sp
