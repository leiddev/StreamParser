/*
 * Leid 2026.7
 *
 * UDP demo using UdpParserAdapter + A55A55AAParser.
 *
 * Demonstrates:
 *   - Binding to a local UDP port
 *   - Receiving data from any sender
 *   - Replying to the last sender (echo behavior)
 *   - Sending data to a specified endpoint
 *
 * Usage:
 *   ./UdpDemo [local_port] [remote_host] [remote_port]
 *
 * Examples:
 *   ./UdpDemo 8000              # Listen on port 8000, no auto-send
 *   ./UdpDemo 8000 127.0.0.1 9000  # Also send test frames to UDP server on 127.0.0.1:9000
 *
 * Note: This is a protocol-agnostic UDP adapter. It does not distinguish
 * between "client" and "server" roles - any endpoint can send/receive.
 */
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <asio.hpp>
#include <sp/UdpParserAdapter.h>
#include "A55A55AAParser.h"

// Every adapter lives in namespace sp.
using namespace sp;

static std::shared_ptr<UdpParserAdapter> g_adapter;

static void onOpen(bool ok,
                   const asio::error_code& ec,
                   const std::string& detail) {
    if (ok) {
        std::cout << "[Open] ok=1 " << detail << std::endl;
    } else {
        std::cerr << "[Open] ok=0 ec=" << ec.message()
                  << " detail=" << detail << std::endl;
    }
}

static void onClose(const std::string& tag) {
    std::cout << "[Close] " << tag << std::endl;
}

int main(int argc, char* argv[]) {
    std::cout << "=== UDP Stream Parser Demo ===" << std::endl;

    // Default: listen on port 8000, no remote
    uint16_t local_port = 8000;
    std::string remote_host;
    uint16_t remote_port = 0;

    if (argc >= 2) local_port = static_cast<uint16_t>(std::stoi(argv[1]));
    if (argc >= 3) remote_host = argv[2];
    if (argc >= 4) remote_port = static_cast<uint16_t>(std::stoi(argv[3]));

    asio::io_context io;

    // 1. Build parser. tag identifies this adapter in logs.
    std::string tag = "udp:" + std::to_string(local_port);
    auto parser = std::make_unique<A55A55AAParser>(tag, 300, 2048);

    // 2. Set frame callback - echo back to sender
    parser->setFrameCallback(
        [&tag](const std::string& cb_tag, const uint8_t* data, size_t len) {
            std::cout << "[Frame] tag=" << cb_tag << " len=" << len << " data=";
            for (size_t i = 0; i < len; ++i) {
                std::printf("%02X ", data[i]);
            }
            std::cout << std::endl;

            // Echo back to the sender using async_write (sends to last_sender)
            if (g_adapter && g_adapter->is_open()) {
                std::vector<uint8_t> response(data, data + len);
                g_adapter->build_and_async_write(response,
                    [](UdpWriteResult r, size_t n,
                       const asio::error_code& /*ec*/,
                       const std::string& detail) {
                        if (r == UdpWriteResult::Ok) {
                            std::cout << "[Echo] sent " << n << " bytes to last sender" << std::endl;
                        } else {
                            std::cerr << "[Echo] failed: " << detail << std::endl;
                        }
                    });
            }
        });

    // 3. Set error callback
    parser->setErrorCallback(
        [](const std::string& cb_tag,
           StreamParser::ErrorCode code,
           const std::string& detail) {
            std::cerr << "[Error] tag=" << cb_tag
                      << " code=" << static_cast<int>(code)
                      << " detail=" << detail << std::endl;
        });

    // 4. Configure adapter
    UdpAdapterConfig config;
    config.timer_period_ms = 30;
    config.recv_buffer_bytes = 4096;
    config.reuse_address = true;

    // 5. Construct adapter and wire callbacks
    g_adapter = std::make_shared<UdpParserAdapter>(
        io, local_port, std::move(parser), config);

    g_adapter->setOpenCallback(onOpen);
    g_adapter->setCloseCallback(onClose);

    // 6. Open (bind to local port and start receiving)
    g_adapter->open();

    // 7. Optionally send initial test frame to remote endpoint
    if (!remote_host.empty() && remote_port > 0) {
        asio::error_code ec;
        asio::ip::address addr = asio::ip::address::from_string(remote_host, ec);
        if (!ec) {
            asio::ip::udp::endpoint remote_ep(addr, remote_port);

            std::cout << "Will send test frames to " << remote_host << ":" << remote_port << std::endl;

            // Use a weak_ptr to avoid lifetime issues
            auto weak_adapter = std::weak_ptr<UdpParserAdapter>(g_adapter);

            // Wait a bit for open to complete, then send test data
            io.post([weak_adapter, remote_ep]() {
                auto adapter = weak_adapter.lock();
                if (!adapter) return;

                // Send a few test frames
                for (int i = 0; i < 3; ++i) {
                    std::vector<uint8_t> payload = {
                        static_cast<uint8_t>(0xAA),
                        static_cast<uint8_t>(0x55 + i),
                        static_cast<uint8_t>(0x55),
                        static_cast<uint8_t>(i * 0x10)
                    };

                    adapter->async_send_to(remote_ep,
                        std::make_shared<std::vector<uint8_t>>(payload),
                        [i](UdpWriteResult r, size_t n,
                            const asio::error_code& /*ec*/,
                            const std::string& detail) {
                            if (r == UdpWriteResult::Ok) {
                                std::cout << "[Init Send #" << i << "] sent " << n << " bytes" << std::endl;
                            } else {
                                std::cerr << "[Init Send #" << i << "] failed: " << detail << std::endl;
                            }
                        });
                }
            });
        } else {
            std::cerr << "Invalid remote host: " << remote_host << std::endl;
        }
    }

    std::cout << "UDP adapter listening on port " << local_port
              << ". Press Ctrl+C to stop." << std::endl;

    try {
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "io_context exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "io_context unknown exception" << std::endl;
    }

    std::cout << "Demo exited." << std::endl;
    return 0;
}
