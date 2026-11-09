/*
 * Leid 2026.7
 */
#pragma once
#ifndef _UDP_TYPES_H_
#define _UDP_TYPES_H_

#include <asio.hpp>
#include <functional>
#include <string>
#include <cstdint>

namespace sp {

// -------------------------------------------------
// 1. Write result enum (business-layer friendly, no asio dependency)
// -------------------------------------------------
enum class UdpWriteResult {
    Ok,           // Datagram sent successfully
    SendFailed,   // Failed to send (socket not open, network error, etc.)
    Cancelled,    // Operation aborted (close() called or adapter destroyed)
    NotBound,     // Socket not bound to a local port
    InvalidArg,   // Invalid argument (null or empty frame)
};

// -------------------------------------------------
// 2. Write completion callback
// -------------------------------------------------
// @param result   Write outcome, business-layer facing
// @param bytes   Actual bytes sent; on Ok == frame->size(), on failure == 0
// @param ec      Raw asio error code; valid only within this callback.
// @param detail  Human-readable error description; non-empty on failure
using UdpWriteCallback = std::function<void(UdpWriteResult         result,
                                            size_t                  bytes,
                                            const asio::error_code& ec,
                                            const std::string&      detail)>;

// -------------------------------------------------
// 3. Open completion callback
// -------------------------------------------------
// Fired when open() completes:
//   ok == true  : socket bound and ready to send/receive
//   ok == false : bind failed (port in use, permission denied, etc.)
// ec and detail describe the failure when ok == false; both are empty on success.
using UdpOpenCallback = std::function<void(bool                      ok,
                                           const asio::error_code&   ec,
                                           const std::string&        detail)>;

// -------------------------------------------------
// 4. Close callback
// -------------------------------------------------
// Called when the socket is closed (either by local close(),
// or error). Tag identifies this adapter (usually "udp:port").
using UdpCloseCallback = std::function<void(const std::string& tag)>;

// -------------------------------------------------
// 5. UDP adapter configuration struct
// -------------------------------------------------
struct UdpAdapterConfig {
    uint32_t timer_period_ms     = 30;    // Frame timeout check period (recommended 20~50ms)
    size_t   recv_buffer_bytes  = 4096;  // async_receive_from buffer size
    size_t   send_buffer_bytes = 4096;   // Socket send buffer size (0 = system default)
    bool     reuse_address      = true;   // Set SO_REUSEADDR before bind
};

}  // namespace sp

#endif  // !_UDP_TYPES_H_
