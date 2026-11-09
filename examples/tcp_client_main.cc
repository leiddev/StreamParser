/*
 * Leidi 2026.7
 *
 * TCP client demo using TcpClientParserAdapter + A55A55AAParser.
 *
 * Demonstrates:
 *   - Connecting to a TCP server with async operations
 *   - Parsing stream data with frame detection
 *   - Echo response to server (auto-reply received frames)
 *   - Connection state callbacks (connect/disconnect)
 *   - TCP keep-alive and NoDelay configuration
 *
 * Usage:
 *   ./TcpClientDemo [host] [port]
 *
 * Examples:
 *   ./TcpClientDemo                      # Default: 10.0.228.249:9000
 *   ./TcpClientDemo 127.0.0.1 8080      # Connect to localhost:8080
 *
 * Note: Auto-reconnect is not wired in v1; reconnect policy lives in the business layer.
 */
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <asio.hpp>
#include <sp/TcpClientParserAdapter.h>
#include "A55A55AAParser.h"

// Every adapter lives in namespace sp.
using namespace sp;

static std::shared_ptr<TcpClientParserAdapter> g_client;

static void onConnect(bool ok,
                      const asio::error_code& ec,
                      const std::string& detail) {
    if (ok) {
        std::cout << "[Connect] ok=1 " << detail << std::endl;
    } else {
        std::cerr << "[Connect] ok=0 ec=" << ec.message()
                  << " detail=" << detail << std::endl;
    }
}

static void onDisconnect(const std::string& tag) {
    std::cout << "[Disconnect] " << tag << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "=== TCP Client Stream Parser Demo ===" << std::endl;

    std::string host = "10.0.228.249";
    uint16_t    port = 9000;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));

    asio::io_context io;

    // 1. Build parser. tag matches the client's host:port so log lines are
    //    easy to correlate with TcpServerDemo's [Frame] output.
    std::string tag = host + ":" + std::to_string(port);
    auto parser = std::make_unique<A55A55AAParser>(tag, 300, 2048);

    parser->setFrameCallback(
        [&tag](const std::string& cb_tag, const uint8_t* data, size_t len) {
            std::cout << "[Frame] tag=" << cb_tag << " len=" << len << " data=";
            for (size_t i = 0; i < len; ++i) {
                std::printf("%02X ", data[i]);
            }
            std::cout << std::endl;
            if (g_client && g_client->state() ==
                TcpClientParserAdapter::State::Connected) {
                std::vector<uint8_t> response(data, data + len);
                g_client->build_and_async_write(response);
            }
        });

    parser->setErrorCallback(
        [](const std::string& cb_tag,
           StreamParser::ErrorCode code,
           const std::string& detail) {
            std::cerr << "[Error] tag=" << cb_tag
                      << " code=" << static_cast<int>(code)
                      << " detail=" << detail << std::endl;
        });

    // 2. Configure client adapter.
    TcpClientConfig client_config;
    client_config.connect_timeout_ms = 5000;
    client_config.no_delay           = true;
    client_config.tcp.enable_keepalive           = true;
    client_config.tcp.keepalive_idle_seconds     = 30;
    client_config.tcp.keepalive_interval_seconds = 5;
    client_config.tcp.keepalive_probes           = 3;
    client_config.tcp.graceful_shutdown          = true;

    // 3. Construct adapter and wire callbacks.
    g_client = std::make_shared<TcpClientParserAdapter>(
        io, host, port, std::move(parser), client_config);

    g_client->setConnectCallback(onConnect);
    g_client->setDisconnectCallback(onDisconnect);

    g_client->connect();

    std::cout << "Client connecting to " << host << ":" << port
              << ". Press Ctrl+C to stop." << std::endl;

    try {
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "io_context exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "io_context unknown exception" << std::endl;
    }

    std::cout << "Client exited." << std::endl;
    return 0;
}