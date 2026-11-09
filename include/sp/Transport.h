/*
 * Transport.h - One uniform, asio-free facade over serial / TCP / UDP.
 *
 * Leid 2026.10
 *
 * The four adapters (SerialParserAdapter, TcpServer + TcpSessionParserAdapter,
 * TcpClientParserAdapter, UdpParserAdapter) each take an asio::io_context and
 * expose their own sockets, sessions and write queues. That is the right level
 * for a host that wants full control, but it forces every business object to
 * own an io_context, a worker thread and a session table.
 *
 * This facade hides all of that behind one interface:
 *
 *     auto transport = CreateTransport(cfg, {}, handler);
 *     transport->start();
 *     transport->send(tag, payload);     // framed by the transport
 *     transport->broadcast(payload);
 *     transport->stop();
 *
 * Design notes
 * ------------
 * - Facade + Abstract Factory: CreateTransport() is the only entry point and
 *   picks the implementation from TransportConfig::kind.
 * - The transport owns its io_context, its worker thread and its per-link send
 *   queue, so the caller never includes <asio.hpp>.
 * - One parser per link: the parser carries per-stream state (frame cache,
 *   inter-byte timer), so a ParserFactory is used instead of a prototype.
 * - Callbacks always run on the transport's io thread. A caller that touches
 *   its own state from a callback must marshal it (queue it, post it, ...).
 * - stop() must not be called from a callback (it joins the io thread).
 *
 * This header has no asio dependency on purpose.
 */
#pragma once
#ifndef _SP_TRANSPORT_H_
#define _SP_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sp/StreamParser.h>

namespace sp {

// -------------------------------------------------
// 1. Which transport to build
// -------------------------------------------------
enum class TransportKind {
    Serial,     // one device, one link, automatic reopen
    TcpServer,  // many clients, all accepted, replies routed by tag
    TcpClient,  // one server, automatic reconnect
    Udp,        // datagrams, many peers, replies by tag
};

// Serial line settings, kept separate from SerialTypes.h so that this header
// stays free of asio (SerialTypes.h includes <asio.hpp>).
enum class LineParity { None, Odd, Even };
enum class LineStopBits { One, OnePointFive, Two };
enum class LineFlowControl { None, Software, Hardware };

// -------------------------------------------------
// 2. Configuration (one struct for the four kinds)
// -------------------------------------------------
struct TransportConfig {
    TransportKind kind = TransportKind::TcpServer;
    std::string   name;                 // Log prefix and default link tag

    // --- serial ---
    std::string    device;               // "/dev/ttyUSB0", "COM3"
    unsigned       baud_rate      = 115200;
    LineParity     parity         = LineParity::None;
    LineStopBits   stop_bits      = LineStopBits::One;
    LineFlowControl flow_control  = LineFlowControl::None;
    uint32_t       reopen_delay_ms = 3000;  // Retry delay when the device is missing

    // --- tcp / udp ---
    std::string   bind_address = "0.0.0.0";  // TcpServer listen address / Udp bind address
    uint16_t      port         = 0;          // TcpServer / Udp local port
    std::string   peer_host;                 // TcpClient host / fixed Udp destination
    uint16_t      peer_port    = 0;          // TcpClient port / fixed Udp destination port
    int           backlog      = 128;
    uint32_t      reconnect_ms = 3000;       // TcpClient reconnect delay (0 = never)

    // --- shared ---
    uint32_t      frame_timeout_ms  = 300;       // Passed to the parser factory
    std::size_t   max_frame_bytes   = 10 * 1024; // Passed to the parser factory
    std::size_t   recv_buffer_bytes = 4096;
    std::size_t   max_links         = 0;         // 0 = unlimited (TcpServer / Udp)
    std::size_t   max_pending_frames = 256;      // Per-link send queue (back pressure)

    // TCP keepalive (tcp_server accepted sockets and tcp_client). Detects a peer
    // that vanished without FIN/RST - power loss, cable pull - in roughly
    // idle + interval * probes seconds instead of waiting for a write to fail.
    // Keepalive idle/interval/probes are applied per socket (SIO_KEEPALIVE_VALS
    // on Windows, TCP_KEEPIDLE/KEEPINTVL/KEEPCNT on Linux).
    bool          keep_alive                 = false;
    int           keepalive_idle_seconds     = 30;
    int           keepalive_interval_seconds = 5;
    int           keepalive_probes           = 3;

    // Close a TCP link that received no bytes for this long. Complements
    // keepalive: it also catches a peer that is reachable but stuck (rebooted
    // process, hung app) and needs no traffic in either direction to fire.
    // 0 disables it. tcp_server and tcp_client only; ignored by serial/udp.
    std::uint32_t link_idle_timeout_ms       = 0;

    // Log link up/down events ("client accepted / closed"). A host that already
    // logs them from on_link_up / on_link_down can turn this off to avoid
    // printing two lines per event. Startup, error and retry logs are not
    // affected by this flag.
    bool          log_link_events   = true;
};

// -------------------------------------------------
// 3. Events
// -------------------------------------------------
// Every callback runs on the transport's io thread.
//
// tag identifies the link:
//   Serial    -> the device name
//   TcpServer -> "ip:port" of the accepted client
//   TcpClient -> "host:port" of the server
//   Udp       -> "ip:port" of the peer (plus "udp:<port>" for the socket itself)
struct TransportHandler {
    // A complete frame arrived (already de-framed by the parser).
    std::function<void(const std::string& tag, const uint8_t* data, std::size_t len)> on_frame;
    // A link became usable (serial opened, client accepted/connected, udp bound).
    std::function<void(const std::string& tag)> on_link_up;
    // A link became unusable. `why` is a human-readable reason.
    std::function<void(const std::string& tag, const std::string& why)> on_link_down;
    // A transport-level problem. `code` is transport specific (< 0 = fatal).
    std::function<void(const std::string& tag, int code, const std::string& detail)> on_error;
    // A frame was handed to the OS (useful for send-side logging). Optional.
    std::function<void(const std::string& tag, const std::vector<uint8_t>& frame)> on_sent;
};

// -------------------------------------------------
// 4. Parser factory
// -------------------------------------------------
// The host owns the protocol: the facade never names a concrete parser.
// The factory is called once per link and once for the encoder, whose tag is
// "<name>#encoder" (or "encoder" when name is empty). Each call must return a
// fresh parser; `tag` lets a host pick the protocol per link if it needs to.
// An empty factory is refused by CreateTransport().
using ParserFactory = std::function<std::unique_ptr<StreamParser>(const std::string& tag)>;

// -------------------------------------------------
// 5. The facade
// -------------------------------------------------
class Transport {
public:
    virtual ~Transport() = default;

    // Bind/open the transport and start its io thread.
    // Returns false when the transport cannot be set up (bad config, bind
    // failure, ...). A serial device that is currently missing does NOT make
    // start() fail: the transport keeps retrying in the background.
    virtual bool start() = 0;

    // Close every link and join the io thread. Idempotent.
    // Must be called from a thread other than the io thread.
    virtual void stop() = 0;

    virtual bool is_running() const = 0;

    // Frame `payload` with the parser of `tag`'s link and queue it.
    // Non-blocking; returns false when the link is unknown or its queue is
    // full (back pressure).
    virtual bool send(const std::string& tag, const std::vector<uint8_t>& payload) = 0;

    // Send the same payload to every known link. Returns false when there is
    // no link or a queue rejected the frame.
    virtual bool broadcast(const std::vector<uint8_t>& payload) = 0;

    // Send to an explicit destination (Udp only). Other kinds return false.
    virtual bool send_to(const std::string& host,
                         uint16_t           port,
                         const std::vector<uint8_t>& payload) = 0;

    // Tags of the links that are currently up.
    virtual std::vector<std::string> links() const = 0;
};

// -------------------------------------------------
// 6. Factory
// -------------------------------------------------
// `parser_factory` is required: it decides the framing on both directions and
// is the only place a concrete parser is named. Returns nullptr when the
// factory is empty or the configuration is invalid.
std::unique_ptr<Transport> CreateTransport(const TransportConfig& cfg,
                                           ParserFactory          parser_factory,
                                           TransportHandler       handler);

}  // namespace sp

#endif  // !_SP_TRANSPORT_H_
