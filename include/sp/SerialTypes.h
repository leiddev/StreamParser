/*
 * Leid 2026.7
 */
#pragma once
#ifndef _SERIAL_TYPES_H_
#define _SERIAL_TYPES_H_

#include <asio.hpp>
#include <functional>
#include <string>
#include <cstdint>

namespace sp {

// -------------------------------------------------
// 1. Serial port configuration struct
// -------------------------------------------------
struct SerialConfig {
    unsigned int baud_rate      = 115200;
    unsigned int character_size = 8;
    
    enum class Parity { None, Odd, Even };
    Parity parity = Parity::None;

    enum class StopBits { One, OnePointFive, Two };
    StopBits stop_bits = StopBits::One;

    enum class FlowControl { None, Software, Hardware };
    FlowControl flow_control = FlowControl::None;
};

// -------------------------------------------------
// 2. Write result enum (business-layer friendly, no asio dependency)
// -------------------------------------------------
enum class SerialWriteResult {
    Ok,          // Atomic frame transmitted successfully
    PortClosed,  // Serial port not open or already closed
    Cancelled,   // Operation aborted (close() called or adapter destroyed)
    IOError,     // Low-level I/O error (see detail)
    InvalidArg,  // Invalid argument (null or empty frame)
};

// -------------------------------------------------
// 3. Write completion callback
// -------------------------------------------------
using SerialWriteCallback = std::function<void(SerialWriteResult   result,
                                          size_t                   bytes,
                                          const asio::error_code&  ec,
                                          const std::string&       detail)>;

}  // namespace sp

#endif  // !_SERIAL_TYPES_H_
