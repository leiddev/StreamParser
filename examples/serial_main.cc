/*
 * Leid 2026.7
 *
 * Serial demo using SerialParserAdapter + A55A55AAParser.
 *
 * Demonstrates:
 *   - Opening a serial port with configurable parameters
 *   - Parsing stream data with frame detection
 *   - Async write operations with callbacks
 *   - Handling platform-specific serial port paths
 *
 * Usage:
 *   ./SerialDemo [port_name] [baud_rate]
 *
 * Examples:
 *   ./SerialDemo              # Use default: COM1 at 115200
 *   ./SerialDemo COM3 9600    # Windows: COM3 at 9600 baud
 *   ./SerialDemo /dev/ttyS0 115200  # Linux: ttyS0 at 115200 baud
 *
 * Note: Default port is COM1 on Windows, /dev/ttyS1 on Linux.
 */
#include <string>
#include <thread>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <memory>
#include <cstdio>
#include "A55A55AAParser.h"
#include <sp/SerialParserAdapter.h>

// Every adapter lives in namespace sp.
using namespace sp;

// Global adapter pointer
std::unique_ptr<SerialParserAdapter> g_adapter;

int main(int argc, char* argv[]) {
    std::cout << "=== Serial Stream Parser Demo ===" << std::endl;

#ifdef _WIN32
    std::string port_name = "COM1";  // Change to your serial port
#else
    std::string port_name = "/dev/ttyS1";  // Change to your serial port
#endif
    uint32_t    baud_rate = 115200;
    if (argc >= 2) port_name = argv[1];
    if (argc >= 3) baud_rate = static_cast<uint32_t>(std::stoi(argv[2]));

    asio::io_context io;

    // 1. Configure serial port parameters
    SerialConfig config;
    config.baud_rate      = baud_rate;
    config.character_size = 8;
    config.parity         = SerialConfig::Parity::None;
    config.stop_bits      = SerialConfig::StopBits::One;
    config.flow_control   = SerialConfig::FlowControl::None;

    // 2. Create specific protocol parser
    auto parser = std::make_unique<A55A55AAParser>(port_name, 300, 2048);
    std::cout << "Serial port: " << port_name << std::endl
              << "Baud rate: " << baud_rate << std::endl;

    parser->setFrameCallback(
        [](const std::string& tag, const uint8_t* data, size_t len) {
            std::cout << "[Frame] tag=" << tag << " len=" << len << " data=";
            for (size_t i = 0; i < len; ++i) {
                std::printf("%02X ", data[i]);
            }
            std::cout << std::endl;
            std::vector<uint8_t> response(data, data + len);
            if (g_adapter)
                g_adapter->build_and_async_write(response);
            // Can push frame to business queue or process directly here
        });

    parser->setErrorCallback(
        [](const std::string& tag, StreamParser::ErrorCode code, const std::string& detail) {
            std::cerr << "[Error] tag=" << tag
                << " code=" << static_cast<int>(code)
                << " detail=" << detail << std::endl;
        });

    try {
        // 3. Inject configuration + parser into adapter
        g_adapter = std::make_unique<SerialParserAdapter>(io, port_name, config, std::move(parser));

        // 4. Test Case: build_and_async_write with callback
        g_adapter->build_and_async_write({ 0xAA, 0xBB, 0xCC },
            [](SerialWriteResult r, size_t n,
                const asio::error_code& /*ec*/, const std::string& detail) {
                if (r == SerialWriteResult::Ok) {
                    std::cout << "[build_and_async_write] sent " << n << " bytes" << std::endl;
                } else {
                    std::cerr << "[build_and_async_write] failed: " << detail << std::endl;
                }
            });

#ifdef __linux
        // Verify close/reopen behavior on Linux only:
        // On Windows COM ports the OS retains the handle briefly after
        // close(), causing immediate reopen to fail with access_denied.
        g_adapter->close();
        g_adapter->open();
#endif

        io.run();
    } catch (const std::exception& e) {
        std::cerr << "io_context exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "io_context unknown exception" << std::endl;
    }

    std::cout << "Demo exited." << std::endl;
    return 0;
}
