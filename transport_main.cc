/*
 * Leid 2026.10
 *
 * Demo of the Transport facade: the program never sees asio, an io_context,
 * a socket or a session. It configures a transport, gets frames and sends
 * payloads back.
 *
 * Usage:
 *   TransportDemo tcp_server [port]
 *   TransportDemo tcp_client <host> <port>
 *   TransportDemo udp <port>
 *   TransportDemo serial <device> [baud]
 *
 * Behavior: every complete frame is printed and echoed back to the link it
 * came from ("[Frame]" / "[Sent]").
 */
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "A55A55AAParser.h"
#include "StreamParserLog.h"
#include "Transport.h"

using namespace sp;

namespace {

void installLogSink() {
    splog::set_level(splog::Level::debug);
    splog::set_callback([](const splog::Record& r) {
        std::cout << "[lib][" << splog::level_name(r.level) << "][" << r.component << "] "
                  << r.message << std::endl;
    });
}

void printUsage() {
    std::cout << "usage:\n"
                 "  TransportDemo tcp_server [port]\n"
                 "  TransportDemo tcp_client <host> <port>\n"
                 "  TransportDemo udp <port>\n"
                 "  TransportDemo serial <device> [baud]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    installLogSink();

    const std::string kind = argv[1];

    TransportConfig config;
    config.name = "demo";
    config.frame_timeout_ms = 300;
    config.max_frame_bytes  = 2048;

    if (kind == "tcp_server") {
        config.kind = TransportKind::TcpServer;
        config.port = (argc >= 3) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;
    } else if (kind == "tcp_client") {
        if (argc < 4) {
            printUsage();
            return 2;
        }
        config.kind      = TransportKind::TcpClient;
        config.peer_host = argv[2];
        config.peer_port = static_cast<uint16_t>(std::atoi(argv[3]));
    } else if (kind == "udp") {
        config.kind = TransportKind::Udp;
        config.port = (argc >= 3) ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;
    } else if (kind == "serial") {
        if (argc < 3) {
            printUsage();
            return 2;
        }
        config.kind      = TransportKind::Serial;
        config.device    = argv[2];
        config.baud_rate = (argc >= 4) ? static_cast<unsigned>(std::atoi(argv[3])) : 115200;
    } else {
        printUsage();
        return 2;
    }

    /* Every callback runs on the transport's io thread; this demo only prints
       and forwards, so it needs no synchronization. */
    std::unique_ptr<Transport> transport;

    TransportHandler handler;
    handler.on_link_up = [](const std::string& tag) {
        std::cout << "[LinkUp] " << tag << std::endl;
    };
    handler.on_link_down = [](const std::string& tag, const std::string& why) {
        std::cout << "[LinkDown] " << tag << " (" << why << ")" << std::endl;
    };
    handler.on_error = [](const std::string& tag, int code, const std::string& detail) {
        std::cerr << "[Error] tag=" << tag << " code=" << code << " detail=" << detail
                  << std::endl;
    };
    handler.on_sent = [](const std::string& tag, const std::vector<uint8_t>& frame) {
        std::cout << "[Sent] tag=" << tag << " len=" << frame.size() << std::endl;
    };
    handler.on_frame = [&transport](const std::string& tag, const uint8_t* data, size_t len) {
        std::cout << "[Frame] tag=" << tag << " len=" << len << " data=";
        for (size_t i = 0; i < len; ++i) {
            std::printf("%02X ", data[i]);
        }
        std::cout << std::endl;

        /* Echo the payload back to the link it came from. */
        if (transport) {
            transport->send(tag, std::vector<uint8_t>(data, data + len));
        }
    };

    /* The facade carries no protocol of its own: the demo injects A55A55AA for
       every link and for the encoder. */
    const uint32_t    frame_timeout_ms = config.frame_timeout_ms;
    const std::size_t max_frame_bytes  = config.max_frame_bytes;
    ParserFactory factory = [frame_timeout_ms, max_frame_bytes](const std::string& tag) {
        return std::unique_ptr<StreamParser>(
            new A55A55AAParser(tag, frame_timeout_ms, max_frame_bytes));
    };

    transport = CreateTransport(config, std::move(factory), handler);
    if (!transport) {
        std::cerr << "cannot create transport" << std::endl;
        return 1;
    }
    if (!transport->start()) {
        std::cerr << "cannot start transport" << std::endl;
        return 1;
    }

    std::cout << "transport started (" << kind << "). Press Ctrl+C to stop." << std::endl;

    /* The transport runs its own io thread; wait for end of input. */
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "quit") {
            break;
        }
        if (line.rfind("send ", 0) == 0 && !transport->links().empty()) {
            const std::string text = line.substr(5);
            transport->broadcast(std::vector<uint8_t>(text.begin(), text.end()));
        }
    }

    transport->stop();
    std::cout << "transport stopped." << std::endl;
    return 0;
}
