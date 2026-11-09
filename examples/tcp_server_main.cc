/*
 * Leid 2026.7
 *
 * TCP server demo using TcpServer + TcpSessionParserAdapter.
 *
 * Demonstrates:
 *   - Accepting multiple TCP client connections
 *   - Parsing stream data with frame detection
 *   - Session management (connect/disconnect tracking)
 *   - Echo response to connected clients
 *   - Keep-alive heartbeat configuration
 *
 * Usage:
 *   ./TcpServerDemo [port]
 *
 * Examples:
 *   ./TcpServerDemo           # Default port 9000
 *   ./TcpServerDemo 8080      # Listen on port 8080
 *
 * Note: Default bind address is 0.0.0.0 (all interfaces).
 */
#include <iostream>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <cstdio>
#include <asio.hpp>
#include <sp/TcpParserServer.h>
#include <sp/TcpSessionParserAdapter.h>
#include <sp/StreamParserLog.h>
#include "A55A55AAParser.h"

// Every adapter lives in namespace sp.
using namespace sp;

static std::unordered_map<std::string, std::weak_ptr<TcpSessionParserAdapter>> g_sessions;
static std::mutex g_sessions_mutex;

// Demo: forward the library's internal diagnostics to this program's logging
// backend. Replace the body with spdlog / glog / log4cxx / a file writer ...
static void installLogSink() {
    splog::set_level(splog::Level::debug);   // Level::off silences the library
    splog::set_callback([](const splog::Record& r) {
        std::cout << "[lib][" << splog::level_name(r.level) << "][" << r.component
                  << "] " << r.message << std::endl;
    });
}

static void onDisconnect(const std::string& tag) {
    std::cout << "[Disconnect] " << tag << std::endl;
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    g_sessions.erase(tag);
}

int main(int argc, char* argv[]) {
    std::cout << "=== TCP Stream Parser Demo ===" << std::endl;

    installLogSink();

    uint16_t port = 9000;
    if (argc >= 2) port = static_cast<uint16_t>(std::stoi(argv[1]));

    asio::io_context io;

    auto session_factory = [&](asio::ip::tcp::socket socket) -> std::shared_ptr<TcpSessionParserAdapter> {
        auto remote_ep = socket.remote_endpoint();
        std::string tag = remote_ep.address().to_string() + ":" +
                          std::to_string(remote_ep.port());

        std::cout << "[Accept] " << tag << std::endl;

        auto parser = std::make_unique<A55A55AAParser>(tag, 300, 2048);

        parser->setFrameCallback(
            [](const std::string& tag, const uint8_t* data, size_t len) {
                std::cout << "[Frame] tag=" << tag << " len=" << len << " data=";
                for (size_t i = 0; i < len; ++i) {
                    std::printf("%02X ", data[i]);
                }
                std::cout << std::endl;
                auto sp = g_sessions[tag].lock();
                if (sp) {
                    std::vector<uint8_t> response(data, data + len);
                    sp->build_and_async_write(response);
                }
            });

        parser->setErrorCallback(
            [](const std::string& tag,
               StreamParser::ErrorCode code,
               const std::string& detail) {
                std::cerr << "[Error] tag=" << tag
                          << " code=" << static_cast<int>(code)
                          << " detail=" << detail << std::endl;
            });

        TcpAdapterConfig adapter_config;
        adapter_config.enable_keepalive = true;
        adapter_config.keepalive_idle_seconds = 30;
        adapter_config.keepalive_interval_seconds = 5;
        adapter_config.keepalive_probes = 3;

        auto adapter = std::make_shared<TcpSessionParserAdapter>(
            io, std::move(socket), tag, std::move(parser), adapter_config);

        adapter->setDisconnectCallback(onDisconnect);
        adapter->build_and_async_write({ 0x01, 0x02, 0x03 });

        {
            std::lock_guard<std::mutex> lock(g_sessions_mutex);
            g_sessions[tag] = adapter;
        }

        return adapter;
    };

    TcpConfig config;
    config.bind_address = "0.0.0.0";
    config.port = port;
    config.backlog = 16;

    TcpServer server(io, config, session_factory);
    server.listen();

    std::cout << "Server started on 0.0.0.0:9000. Press Ctrl+C to stop." << std::endl;
    
    try {
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "io_context exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "io_context unknown exception" << std::endl;
    }

    std::cout << "Server exited." << std::endl;
    return 0;
}
