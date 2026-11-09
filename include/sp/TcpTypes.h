/*
 * Leid 2026.7
 */
#pragma once
#ifndef _TCP_TYPES_H_
#define _TCP_TYPES_H_

#include <asio.hpp>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace sp {

// -------------------------------------------------
// 1. Write result enum (business-layer friendly, no asio dependency)
// -------------------------------------------------
enum class TcpWriteResult {
    Ok,           // Atomic frame transmitted successfully
    Disconnected, // Socket closed or remote end disconnected
    Cancelled,    // Operation aborted (close() called or adapter destroyed)
    IOError,      // Low-level I/O error (see detail)
    InvalidArg,   // Invalid argument (null or empty frame)
};

// -------------------------------------------------
// 2. Write completion callback
// -------------------------------------------------
// @param result   Write outcome, business-layer facing
// @param bytes    Actual bytes transmitted; on Ok == frame.size(),
//                 on failure == 0 (frame is atomic)
// @param ec       Raw asio error code; valid only within this callback.
//                 Do not capture or hold it asynchronously.
// @param detail   Human-readable error description; non-empty on failure
using TcpWriteCallback = std::function<void(TcpWriteResult  result,
                                         size_t             bytes,
                                         const asio::error_code& ec,
                                         const std::string&      detail)>;

// -------------------------------------------------
// 3. Disconnect callback
// -------------------------------------------------
// Called when the connection is closed (either by remote disconnect,
// local close(), or error). Tag identifies the connection.
// Note: eof is considered normal disconnect, not an error.
using DisconnectCallback = std::function<void(const std::string& tag)>;

// -------------------------------------------------
// 4. TCP adapter configuration struct
// -------------------------------------------------
struct TcpAdapterConfig {
    uint32_t timer_period_ms     = 30;    // Frame timeout check period (recommended 20~50ms)
    size_t   recv_buffer_bytes  = 4096;  // async_read_some buffer size
    bool     graceful_shutdown  = true;  // Call shutdown() before close()

    // Keepalive configuration (detects silent disconnects)
    bool     enable_keepalive           = false;  // Enable TCP keepalive
    int      keepalive_idle_seconds     = 30;     // Time before first probe
    int      keepalive_interval_seconds = 5;      // Interval between probes
    int      keepalive_probes           = 3;     // Number of probes before declaring dead

    // Link idle timeout: close the session when no bytes arrive at all for this
    // long. Detect the dead peer that keepalive cannot see (a live but hung
    // peer, a half-open proxy) without waiting for a write to fail.
    // 0 disables the check; the frame timeout (timer_period_ms + parser) only
    // covers a frame that started and stopped mid-way.
    uint32_t idle_timeout_ms            = 0;
};

// -------------------------------------------------
// 5. TCP server configuration struct
// -------------------------------------------------
struct TcpConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t    port        = 9000;
    int         backlog     = 128;
};

// -------------------------------------------------
// 6. Connect completion callback (TCP client)
// -------------------------------------------------
// Fired exactly once per connect() invocation:
//   ok == true  : socket entered Connected state
//   ok == false : connect failed (timeout, host parse failure, refused, ...)
// ec and detail describe the failure when ok == false; both are empty on success.
using ConnectCallback = std::function<void(bool ok,
                                           const asio::error_code& ec,
                                           const std::string& detail)>;

// -------------------------------------------------
// 7. TCP client configuration struct
// -------------------------------------------------
// Composes TcpAdapterConfig (timer period, recv buffer, keepalive, graceful shutdown)
// with client-specific knobs (connect timeout, TCP_NODELAY).
struct TcpClientConfig {
    uint32_t         connect_timeout_ms = 5000;  // async_connect timeout
    bool             no_delay           = true;  // TCP_NODELAY on success
    TcpAdapterConfig tcp                = {};     // reuses recv_buffer_bytes, keepalive, etc.
};

}  // namespace sp

#endif  // !_TCP_TYPES_H_
