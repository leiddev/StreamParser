/*
 * Reliability tests for the Transport facade (sp::Transport, sp/Transport.h).
 *
 * The facade hides an io thread, a link table and a per-link send queue behind
 * four callbacks. The e2e scripts prove that one frame survives a real socket;
 * these tests pin down the behaviour the README promises around it: start/stop
 * contracts, tag routing, sticky and split frames, frame timeout, the send
 * budget and back pressure, link admission, error codes, and the reconnect and
 * idle-timeout paths of the TCP client.
 *
 * Every kind is driven in-process: the transport under test on one side, a
 * bare asio socket standing in for the far end on the other. Serial needs a
 * real device and lives in transport_serial_test.cc.
 */
#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/TransportTestKit.h"

namespace {

using sptest::bytes;
using sptest::ErrorEvent;
using sptest::FakeParser;
using sptest::Frame;
using sptest::kWaitMs;
using sptest::Recorder;
using sptest::sleep_ms;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "[OK]   " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

// Error codes the facade reports. Written out here rather than shared with the
// implementation, so a change there shows up as a failing test.
constexpr int kEncodedTooLarge       = -2;
constexpr int kListenFailed          = -5;
constexpr int kMaxLinks              = -6;
constexpr int kResolveFailed         = -7;
constexpr int kClientNotConfigured   = -9;

bool has_error(Recorder& rec, int code) {
    for (const ErrorEvent& e : rec.errors()) {
        if (e.code == code) {
            return true;
        }
    }
    return false;
}

std::string error_detail(Recorder& rec, int code) {
    for (const ErrorEvent& e : rec.errors()) {
        if (e.code == code) {
            return e.detail;
        }
    }
    return std::string();
}

// -------------------------------------------------
// A bare asio TCP peer that reads on a thread of its own
// -------------------------------------------------
class TcpPeer {
public:
    ~TcpPeer() { close(); }

    asio::error_code connect(asio::io_context& io, const std::string& host, uint16_t port) {
        asio::error_code ec;
        socket_.reset(new asio::ip::tcp::socket(io));
        socket_->connect(asio::ip::tcp::endpoint(asio::ip::make_address(host, ec), port), ec);
        if (!ec) {
            start_read();
        }
        return ec;
    }

    // Adopts an already accepted socket.
    void adopt(asio::ip::tcp::socket socket) {
        socket_.reset(new asio::ip::tcp::socket(std::move(socket)));
        start_read();
    }

    // Waits for n more bytes than were already consumed and returns just those,
    // so successive reads line up with successive writes.
    std::vector<uint8_t> read_n(std::size_t n, int timeout_ms = kWaitMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mutex_);
        while (rx_.size() < consumed_ + n) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                break;
            }
        }
        const std::size_t      take = std::min(n, rx_.size() - consumed_);
        std::vector<uint8_t>   out(rx_.begin() + static_cast<std::ptrdiff_t>(consumed_),
                                   rx_.begin() + static_cast<std::ptrdiff_t>(consumed_ + take));
        consumed_ += take;
        return out;
    }

    void write(const std::vector<uint8_t>& data) {
        if (!socket_) {
            return;
        }
        asio::error_code ec;
        asio::write(*socket_, asio::buffer(data), ec);
    }

    void close() {
        if (!socket_) {
            return;
        }
        asio::error_code ec;
        socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_->close(ec);
        socket_.reset();
    }

    // Break the connection with a RST instead of a FIN.
    void abort() {
        if (!socket_) {
            return;
        }
        asio::error_code ec;
        socket_->set_option(asio::socket_base::linger(true, 0), ec);
        socket_->close(ec);
        socket_.reset();
    }

private:
    void start_read() {
        if (!socket_) {
            return;
        }
        std::shared_ptr<asio::ip::tcp::socket> socket = socket_;
        auto buf = std::make_shared<std::array<uint8_t, 4096>>();
        socket->async_read_some(
            asio::buffer(*buf),
            [this, socket, buf](const asio::error_code& ec, std::size_t n) {
                if (ec) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    rx_.insert(rx_.end(), buf->begin(), buf->begin() + n);
                }
                cv_.notify_all();
                start_read();
            });
    }

    std::shared_ptr<asio::ip::tcp::socket> socket_;
    std::mutex                             mutex_;
    std::condition_variable                cv_;
    std::vector<uint8_t>                   rx_;
    std::size_t                            consumed_ = 0;
};

// A loopback TCP server the tests can start and stop, so the client's
// reconnect path has a peer to come back to.
class TcpLoop {
public:
    explicit TcpLoop(asio::io_context& io) : io_(io), acceptor_(io) {}

    bool start() {
        asio::error_code ec;
        acceptor_.open(asio::ip::tcp::v4(), ec);
        if (ec) {
            return false;
        }
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        acceptor_.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port_), ec);
        if (ec) {
            acceptor_.close(ec);
            return false;
        }
        port_ = acceptor_.local_endpoint(ec).port();
        acceptor_.listen(8, ec);
        if (ec) {
            return false;
        }
        start_accept();
        return true;
    }

    // Takes the next connection, waiting at most timeout_ms.
    std::shared_ptr<TcpPeer> accept(int timeout_ms = kWaitMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!pending_.empty()) {
                asio::ip::tcp::socket socket = std::move(pending_.front());
                pending_.pop_front();
                lock.unlock();
                std::shared_ptr<TcpPeer> peer(new TcpPeer());
                peer->adopt(std::move(socket));
                return peer;
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return std::shared_ptr<TcpPeer>();
            }
        }
    }

    // Takes the listener down, so the client's next attempt fails.
    void stop() {
        asio::error_code ec;
        acceptor_.close(ec);
        acceptor_.cancel(ec);
    }

    uint16_t port() const { return port_; }

private:
    void start_accept() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_);
        acceptor_.async_accept(*socket, [this, socket](const asio::error_code& ec) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ec) {
                    pending_.push_back(std::move(*socket));
                }
            }
            cv_.notify_all();
            if (!ec) {
                start_accept();
            }
        });
    }

    asio::io_context&                 io_;
    asio::ip::tcp::acceptor           acceptor_;
    uint16_t                          port_ = 0;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    std::deque<asio::ip::tcp::socket> pending_;
};

// The io thread the bare peers run on.
class PeerIo {
public:
    PeerIo() : guard_(asio::make_work_guard(io_)) {
        thread_ = std::thread([this]() { io_.run(); });
    }
    ~PeerIo() {
        guard_.reset();
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    asio::io_context& io() { return io_; }

private:
    asio::io_context                                           io_;
    asio::executor_work_guard<asio::io_context::executor_type> guard_;
    std::thread                                                thread_;
};

// -------------------------------------------------
// Configuration helpers
// -------------------------------------------------
sp::TransportConfig tcp_server_config(uint16_t port) {
    sp::TransportConfig cfg;
    cfg.kind         = sp::TransportKind::TcpServer;
    cfg.name         = "srv";
    cfg.bind_address = "127.0.0.1";
    cfg.port         = port;
    cfg.max_links    = 0;
    return cfg;
}

sp::TransportConfig tcp_client_config(const std::string& host, uint16_t port) {
    sp::TransportConfig cfg;
    cfg.kind      = sp::TransportKind::TcpClient;
    cfg.name      = "cli";
    cfg.peer_host = host;
    cfg.peer_port = port;
    return cfg;
}

sp::TransportConfig udp_config(uint16_t port) {
    sp::TransportConfig cfg;
    cfg.kind         = sp::TransportKind::Udp;
    cfg.name         = "udp";
    cfg.bind_address = "127.0.0.1";
    cfg.port         = port;
    return cfg;
}

// The facade does not expose the port it bound, so a test that needs to
// connect to it asks the OS for a free one first.
uint16_t free_tcp_port(asio::io_context& io) {
    asio::ip::tcp::acceptor probe(io,
                                  asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const uint16_t port = probe.local_endpoint().port();
    asio::error_code ec;
    probe.close(ec);
    return port;
}

// The UDP port the socket bound when the config asked for 0 is only visible
// in the tag the facade announces: "udp:<port>".
uint16_t port_from_udp_tag(const std::string& tag) {
    if (tag.rfind("udp:", 0) != 0) {
        return 0;
    }
    return static_cast<uint16_t>(std::stoi(tag.substr(4)));
}

// Everything a test needs: the recorder, the transport, and the tidying up.
struct Facade {
    Recorder                       rec;
    std::unique_ptr<sp::Transport> transport;

    ~Facade() {
        if (transport) {
            transport->stop();
        }
    }

    bool build(const sp::TransportConfig& cfg,
               sp::ParserFactory factory = sp::ParserFactory()) {
        if (!factory) {
            factory = sptest::fake_factory(cfg.frame_timeout_ms, cfg.max_frame_bytes);
        }
        transport = sp::CreateTransport(cfg, std::move(factory), rec.handler());
        return transport != nullptr;
    }

    std::string first_link_up() {
        std::vector<std::string> ups = rec.link_ups();
        return ups.empty() ? std::string() : ups[0];
    }
};

// -------------------------------------------------
// Factory and lifecycle
// -------------------------------------------------
void test_factory_requires_parser() {
    Recorder rec;
    std::unique_ptr<sp::Transport> t =
        sp::CreateTransport(tcp_server_config(0), sp::ParserFactory(), rec.handler());
    check(t == nullptr, "CreateTransport refuses an empty parser factory");
}

void test_lifecycle_is_idempotent() {
    Facade f;
    check(f.build(tcp_server_config(0)), "lifecycle: CreateTransport accepts a factory");
    check(f.transport->start(), "lifecycle: start() succeeds on a kernel-chosen port");
    check(f.transport->is_running(), "lifecycle: is_running() is true after start()");
    check(f.transport->start(), "lifecycle: a second start() is a no-op returning true");

    f.transport->stop();
    check(!f.transport->is_running(), "lifecycle: is_running() is false after stop()");
    f.transport->stop();
    check(true, "lifecycle: a second stop() is harmless");

    check(!f.transport->send("nobody", bytes("x")), "lifecycle: send() after stop() returns false");
    check(!f.transport->broadcast(bytes("x")), "lifecycle: broadcast() after stop() returns false");
    check(f.transport->links().empty(), "lifecycle: links() is empty after stop()");
}

// -------------------------------------------------
// TCP server: receiving
// -------------------------------------------------
void test_tcp_server_receive_shapes() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    Facade f;
    check(f.build(tcp_server_config(port)), "tcp_server: transport built");
    check(f.transport->start(), "tcp_server: listening");

    TcpPeer peer;
    check(peer.connect(peer_io.io(), "127.0.0.1", port) == asio::error_code(),
          "tcp_server: a client can connect");
    check(f.rec.wait_link_ups(1),
          "tcp_server: on_link_up fires for the accepted connection");

    const std::string tag = f.first_link_up();
    check(tag.rfind("127.0.0.1:", 0) == 0, "tcp_server: the link tag is the client's ip:port");

    // Sticky: two frames in one write arrive as two frames.
    const std::vector<uint8_t> a    = FakeParser::frame(bytes("aa"));
    const std::vector<uint8_t> b    = FakeParser::frame(bytes("bbb"));
    std::vector<uint8_t>       both = a;
    both.insert(both.end(), b.begin(), b.end());
    peer.write(both);
    check(f.rec.wait_frames(2),
          "tcp_server: two frames in one write are both delivered");
    {
        const std::vector<Frame> frames = f.rec.frames();
        check(frames.size() == 2 && sptest::as_string(frames[0].payload) == "aa" &&
                  sptest::as_string(frames[1].payload) == "bbb",
              "tcp_server: sticky frames keep their order and payloads");
        check(frames.size() == 2 && frames[0].tag == tag && frames[1].tag == tag,
              "tcp_server: every frame is tagged with its link");
    }

    // Split: one frame written in three pieces arrives once.
    f.rec.clear();
    const std::vector<uint8_t> c = FakeParser::frame(bytes("split"));
    peer.write(std::vector<uint8_t>(c.begin(), c.begin() + 2));
    sleep_ms(30);
    peer.write(std::vector<uint8_t>(c.begin() + 2, c.begin() + 4));
    sleep_ms(30);
    peer.write(std::vector<uint8_t>(c.begin() + 4, c.end()));
    check(f.rec.wait_frames(1),
          "tcp_server: a frame split across writes still arrives once");
    {
        const std::vector<Frame> frames = f.rec.frames();
        check(frames.size() == 1 && sptest::as_string(frames[0].payload) == "split",
              "tcp_server: the split frame's payload is intact");
    }

    // Bad checksum: reported, and the link stays up.
    f.rec.clear();
    std::vector<uint8_t> bad = c;
    bad.back() = static_cast<uint8_t>(bad.back() ^ 0xFF);
    peer.write(bad);
    check(f.rec.wait_error_code(static_cast<int>(sp::StreamParser::ErrorCode::ChecksumFail)),
          "tcp_server: a bad checksum is reported as ChecksumFail");
    check(f.rec.link_down_count() == 0, "tcp_server: a bad frame does not take the link down");

    f.rec.clear();
    peer.write(FakeParser::frame(bytes("after")));
    check(f.rec.wait_frames(1),
          "tcp_server: the frame after a bad one is still delivered");

    peer.close();
    check(f.rec.wait_link_downs(1),
          "tcp_server: a closed client fires on_link_down once");
    check(f.transport->links().empty(), "tcp_server: the closed link is gone from links()");
}

void test_tcp_server_frame_timeout() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.frame_timeout_ms    = 100;

    Facade f;
    check(f.build(cfg), "tcp_server: transport built for the frame-timeout case");
    check(f.transport->start(), "tcp_server: listening for the frame-timeout case");

    TcpPeer peer;
    peer.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1),
          "tcp_server: the timeout peer is connected");

    const std::vector<uint8_t> frame = FakeParser::frame(bytes("never-finished"));
    peer.write(std::vector<uint8_t>(frame.begin(), frame.begin() + 3));
    check(f.rec.wait_error_code(static_cast<int>(sp::StreamParser::ErrorCode::Timeout)),
          "tcp_server: a half frame times out as ErrorCode::Timeout");
}

// -------------------------------------------------
// TCP server: sending
// -------------------------------------------------
void test_tcp_server_send_paths() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    Facade f;
    check(f.build(tcp_server_config(port)), "tcp_server: transport built for the send cases");
    check(f.transport->start(), "tcp_server: listening for the send cases");

    TcpPeer first;
    first.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1),
          "tcp_server: first client accepted");
    const std::string tag = f.first_link_up();

    check(f.transport->send(tag, bytes("hello")), "tcp_server: send() to a known tag returns true");
    {
        const std::vector<uint8_t> want = FakeParser::frame(bytes("hello"));
        check(first.read_n(want.size()) == want,
              "tcp_server: the peer receives exactly the framed payload");
    }
    check(f.rec.wait_sent(1),
          "tcp_server: on_sent fires once the OS took the frame");

    check(!f.transport->send("127.0.0.1:1", bytes("x")),
          "tcp_server: send() to an unknown tag returns false");

    TcpPeer second;
    second.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(2),
          "tcp_server: second client accepted");
    check(f.transport->links().size() == 2, "tcp_server: links() lists both clients");

    check(f.transport->broadcast(bytes("all")), "tcp_server: broadcast() returns true with two links");
    {
        const std::vector<uint8_t> want = FakeParser::frame(bytes("all"));
        check(first.read_n(want.size()) == want, "tcp_server: broadcast reaches the first client");
        check(second.read_n(want.size()) == want, "tcp_server: broadcast reaches the second client");
    }

    first.close();
    second.close();
}

void test_send_budget() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    // A frame costs 4 bytes of framing on top of the payload (0x7E, two length
    // bytes, the xor), so a budget of 64 refuses a 61-byte payload and accepts
    // a 60-byte one.
    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.max_frame_bytes     = 64;

    Facade f;
    check(f.build(cfg), "tcp_server: transport built with max_frame_bytes = 64");
    check(f.transport->start(), "tcp_server: listening with max_frame_bytes = 64");

    TcpPeer peer;
    peer.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1), "tcp_server: the peer for the send-budget case is accepted");
    const std::string tag = f.first_link_up();

    const std::vector<uint8_t> fits(60, 0x41);
    check(f.transport->send(tag, fits), "tcp_server: a payload whose frame fits is accepted");

    f.rec.clear();
    const std::vector<uint8_t> too_big(61, 0x41);
    check(!f.transport->send(tag, too_big), "tcp_server: an oversized payload is refused");
    check(has_error(f.rec, kEncodedTooLarge), "tcp_server: the refusal is reported with code -2");
    check(error_detail(f.rec, kEncodedTooLarge).find("max_frame_bytes") != std::string::npos,
          "tcp_server: the refusal names max_frame_bytes");

    check(!f.transport->broadcast(too_big), "tcp_server: broadcast() refuses it too");
    check(!f.transport->send_to("127.0.0.1", port, too_big),
          "tcp_server: send_to() refuses it too, even though it is UDP-only");
}

void test_parser_refusal_is_reported() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    // A parser that refuses on its own must not turn the transport silent.
    Facade f;
    check(f.build(tcp_server_config(port), sptest::fake_factory(300, 10 * 1024, true)),
          "tcp_server: transport built with a parser that refuses to encode");
    check(f.transport->start(), "tcp_server: listening with a refusing parser");

    TcpPeer peer;
    peer.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1),
          "tcp_server: the peer is accepted for the refusing-parser case");

    check(!f.transport->send(f.first_link_up(), bytes("x")),
          "tcp_server: a parser that refuses to encode makes send() return false");
    check(has_error(f.rec, kEncodedTooLarge),
          "tcp_server: the parser's refusal is reported with code -2 as well");
}

void test_tcp_server_max_links() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.max_links           = 1;

    Facade f;
    check(f.build(cfg), "tcp_server: transport built with max_links = 1");
    check(f.transport->start(), "tcp_server: listening with max_links = 1");

    TcpPeer first;
    first.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1),
          "tcp_server: at max_links the first client is still accepted");

    TcpPeer second;
    second.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_error_code(kMaxLinks),
          "tcp_server: a connection past max_links is refused with code -6");
    check(f.rec.link_up_count() == 1, "tcp_server: the refused connection never reports link_up");
    check(f.transport->links().size() == 1, "tcp_server: links() still has one entry");
}

void test_tcp_server_link_down_once() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    Facade f;
    check(f.build(tcp_server_config(port)), "tcp_server: transport built for the reset case");
    check(f.transport->start(), "tcp_server: listening for the reset case");

    TcpPeer peer;
    peer.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1),
          "tcp_server: the peer to reset is accepted");
    const std::string tag = f.first_link_up();

    // A write into a reset connection fails; the write failure and the socket
    // error that follows must not report the same link loss twice.
    peer.abort();
    sleep_ms(100);
    f.transport->send(tag, bytes("gone"));

    check(f.rec.wait_link_downs(1),
          "tcp_server: a reset peer is reported as link down");
    sleep_ms(300);
    check(f.rec.link_down_count() == 1,
          "tcp_server: link down is reported exactly once after a write failure");
    check(f.rec.link_ups().size() == 1, "tcp_server: no spurious link_up after the reset");
}

void test_tcp_server_start_failure() {
    // A bind address that is not on this machine makes bind() fail on every
    // platform. A port conflict does not: the server sets SO_REUSEADDR, and on
    // Linux the second bind then succeeds.
    sp::TransportConfig cfg = tcp_server_config(0);
    cfg.bind_address        = "192.0.2.1";   // TEST-NET-1, never local

    Facade f;
    check(f.build(cfg), "tcp_server: a transport for an unbindable address is still created");
    check(!f.transport->start(), "tcp_server: start() returns false when bind fails");
    check(has_error(f.rec, kListenFailed), "tcp_server: the bind failure is reported with code -5");
    check(!f.transport->is_running(), "tcp_server: is_running() is false after a failed start()");
}

void test_stop_from_callback_does_not_hang() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    // The README forbids stop() from a callback because it joins the io
    // thread. The facade notices and refuses to deadlock; this pins that the
    // transport is still stoppable from a real caller afterwards.
    sp::TransportConfig cfg = tcp_server_config(port);
    Recorder                       rec;
    std::unique_ptr<sp::Transport> transport;
    std::atomic<int>               called{0};

    sp::TransportHandler h = rec.handler();
    h.on_frame = [&transport, &called](const std::string&, const uint8_t*, std::size_t) {
        called.store(1);
        if (transport) {
            transport->stop();
        }
    };
    transport = sp::CreateTransport(cfg, sptest::fake_factory(), h);
    check(transport != nullptr && transport->start(),
          "tcp_server: transport started for the stop()-from-callback case");

    TcpPeer peer;
    peer.connect(peer_io.io(), "127.0.0.1", port);
    peer.write(FakeParser::frame(bytes("stop")));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (called.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        sleep_ms(10);
    }
    check(called.load() == 1, "tcp_server: the frame arrives, so the callback ran");

    transport->stop();
    check(!transport->is_running(),
          "tcp_server: stop() from another thread still works after the refused call");
}

// -------------------------------------------------
// TCP server: back pressure
// -------------------------------------------------
void test_tcp_server_back_pressure() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.max_pending_frames  = 1;
    cfg.max_frame_bytes     = 4096;
    cfg.recv_buffer_bytes   = 1024;

    Facade f;
    check(f.build(cfg), "tcp_server: transport built with max_pending_frames = 1");
    check(f.transport->start(), "tcp_server: listening for the back-pressure case");

    TcpPeer peer;
    peer.connect(peer_io.io(), "127.0.0.1", port);
    check(f.rec.wait_link_ups(1),
          "tcp_server: the slow peer is accepted");
    const std::string tag = f.first_link_up();

    // The peer never reads, so once the socket buffers fill up the writer stays
    // busy and the queue with room for one frame is what rejects the rest.
    const std::vector<uint8_t> chunk(1024, 0x5A);
    bool saw_refusal = false;
    for (int i = 0; i < 4096 && !saw_refusal; ++i) {
        saw_refusal = !f.transport->send(tag, chunk);
    }
    check(saw_refusal, "tcp_server: a full send queue makes send() return false");

    // No error event for back pressure: the return value is the contract.
    check(!has_error(f.rec, kEncodedTooLarge),
          "tcp_server: back pressure is not reported as an encoding error");
}

// -------------------------------------------------
// TCP client
// -------------------------------------------------
const std::string client_tag(uint16_t port) {
    return "127.0.0.1:" + std::to_string(port);
}

void test_tcp_client_roundtrip() {
    PeerIo peer_io;

    TcpLoop loop(peer_io.io());
    check(loop.start(), "tcp_client: loopback server started on a kernel-chosen port");

    Facade f;
    check(f.build(tcp_client_config("127.0.0.1", loop.port())), "tcp_client: transport built");
    check(f.transport->start(), "tcp_client: start() succeeds while the server waits");

    std::shared_ptr<TcpPeer> peer = loop.accept();
    check(peer.get() != nullptr, "tcp_client: the loopback server accepted the connection");
    check(f.rec.wait_link_ups(1),
          "tcp_client: on_link_up fires once connected");
    check(f.first_link_up() == client_tag(loop.port()), "tcp_client: the link tag is host:port");

    peer->write(FakeParser::frame(bytes("in")));
    check(f.rec.wait_frames(1),
          "tcp_client: a frame from the server arrives");
    {
        const std::vector<Frame> frames = f.rec.frames();
        check(frames.size() == 1 && sptest::as_string(frames[0].payload) == "in",
              "tcp_client: the received payload is intact");
    }

    check(f.transport->send(client_tag(loop.port()), bytes("out")),
          "tcp_client: send() returns true on the connected link");
    {
        const std::vector<uint8_t> want = FakeParser::frame(bytes("out"));
        check(peer->read_n(want.size()) == want,
              "tcp_client: the server receives the framed payload");
    }

    check(!f.transport->send_to("127.0.0.1", 9, bytes("x")),
          "tcp_client: send_to() is refused, it is UDP-only");
}

void test_tcp_client_disconnect_detected() {
    PeerIo peer_io;

    TcpLoop loop(peer_io.io());
    check(loop.start(), "tcp_client: loopback server started for the disconnect case");

    Facade f;
    check(f.build(tcp_client_config("127.0.0.1", loop.port())), "tcp_client: transport built");
    check(f.transport->start(), "tcp_client: connected");

    std::shared_ptr<TcpPeer> peer = loop.accept();
    check(peer.get() != nullptr, "tcp_client: the peer accepted");
    check(f.rec.wait_link_ups(1),
          "tcp_client: link_up seen before the disconnect");

    peer->close();
    check(f.rec.wait_link_downs(1),
          "tcp_client: a closed server is reported as link down");
    check(f.transport->links().empty(), "tcp_client: the link is gone from links()");
}

void test_tcp_client_reconnects() {
    PeerIo peer_io;

    TcpLoop loop(peer_io.io());
    check(loop.start(), "tcp_client: loopback server started for the reconnect case");

    sp::TransportConfig cfg = tcp_client_config("127.0.0.1", loop.port());
    cfg.reconnect_ms        = 100;

    Facade f;
    check(f.build(cfg), "tcp_client: transport built with reconnect_ms = 100");
    check(f.transport->start(), "tcp_client: start() succeeds");

    std::shared_ptr<TcpPeer> first = loop.accept();
    check(first.get() != nullptr, "tcp_client: first connection accepted");
    check(f.rec.wait_link_ups(1),
          "tcp_client: first link_up seen");

    first->close();
    check(f.rec.wait_link_downs(1),
          "tcp_client: a closed server reports link down");

    // The listener is still up, so the client must find its way back on its
    // own and announce the link again.
    check(f.rec.wait_link_ups(2, 5000), "tcp_client: the client reconnects without help");

    std::shared_ptr<TcpPeer> second = loop.accept();
    check(second.get() != nullptr, "tcp_client: the reconnect lands on the loopback server");

    second->write(FakeParser::frame(bytes("again")));
    check(f.rec.wait_frames(1),
          "tcp_client: frames flow again after the reconnect");
}

void test_tcp_client_no_reconnect() {
    PeerIo peer_io;

    TcpLoop loop(peer_io.io());
    check(loop.start(), "tcp_client: loopback server started for reconnect_ms = 0");

    sp::TransportConfig cfg = tcp_client_config("127.0.0.1", loop.port());
    cfg.reconnect_ms        = 0;

    Facade f;
    check(f.build(cfg), "tcp_client: transport built with reconnect_ms = 0");
    check(f.transport->start(), "tcp_client: start() succeeds");

    std::shared_ptr<TcpPeer> peer = loop.accept();
    check(peer.get() != nullptr, "tcp_client: the peer accepted");
    check(f.rec.wait_link_ups(1),
          "tcp_client: link_up seen before the disconnect");

    peer->close();
    check(f.rec.wait_link_downs(1),
          "tcp_client: the disconnect is reported with reconnect_ms = 0");
    sleep_ms(800);
    check(f.rec.link_up_count() == 1,
          "tcp_client: reconnect_ms = 0 keeps the client from reconnecting");
}

void test_tcp_client_idle_timeout() {
    PeerIo peer_io;

    TcpLoop loop(peer_io.io());
    check(loop.start(), "tcp_client: loopback server started for the idle case");

    sp::TransportConfig cfg = tcp_client_config("127.0.0.1", loop.port());
    cfg.link_idle_timeout_ms = 200;
    // Let the idle timer be the only thing that acts: a reconnect would
    // otherwise bring the link straight back and hide the timeout again.
    cfg.reconnect_ms         = 0;

    Facade f;
    check(f.build(cfg), "tcp_client: transport built with link_idle_timeout_ms = 200");
    check(f.transport->start(), "tcp_client: connected for the idle case");

    std::shared_ptr<TcpPeer> peer = loop.accept();
    check(peer.get() != nullptr, "tcp_client: the silent peer accepted");
    check(f.rec.wait_link_ups(1),
          "tcp_client: link_up seen before going silent");

    check(f.rec.wait_link_downs(1, 5000),
          "tcp_client: a silent link is closed by link_idle_timeout_ms");
    check(f.rec.wait_link_ups(1, 500) && f.rec.link_up_count() == 1,
          "tcp_client: the idle close is not followed by a reconnect");
}

void test_tcp_client_config_errors() {
    {
        sp::TransportConfig    cfg = tcp_client_config("", 0);
        Facade                 f;
        check(f.build(cfg), "tcp_client: transport built with peer_host empty");
        check(!f.transport->start(), "tcp_client: an empty peer_host makes start() fail");
        check(has_error(f.rec, kClientNotConfigured),
              "tcp_client: the missing peer is reported with code -9");
        check(!f.transport->is_running(), "tcp_client: is_running() is false after the failure");
    }
    {
        sp::TransportConfig cfg = tcp_client_config("127.0.0.1", 0);
        Facade              f;
        check(f.build(cfg), "tcp_client: transport built with peer_port = 0");
        check(!f.transport->start(), "tcp_client: peer_port = 0 makes start() fail");
        check(has_error(f.rec, kClientNotConfigured),
              "tcp_client: peer_port = 0 is reported with code -9");
    }
}

// -------------------------------------------------
// UDP
// -------------------------------------------------
class UdpPeer {
public:
    explicit UdpPeer(asio::io_context& io) : socket_(io) {
        asio::error_code ec;
        socket_.open(asio::ip::udp::v4(), ec);
        socket_.bind(asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
        socket_.non_blocking(true, ec);
        port_ = socket_.local_endpoint(ec).port();
    }

    void send_to(const std::vector<uint8_t>& data, uint16_t port) {
        asio::error_code ec;
        socket_.send_to(asio::buffer(data),
                        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port), 0, ec);
    }

    // Reads one datagram, or nothing when the timeout runs out. UDP has no
    // portable receive timeout through asio, so poll in non-blocking mode.
    std::vector<uint8_t> receive(int timeout_ms = kWaitMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::vector<uint8_t> buf(4096);
        while (std::chrono::steady_clock::now() < deadline) {
            asio::error_code  ec;
            const std::size_t n = socket_.receive(asio::buffer(buf), 0, ec);
            if (!ec) {
                buf.resize(n);
                return buf;
            }
            if (ec != asio::error::would_block && ec != asio::error::try_again) {
                return {};
            }
            sleep_ms(5);
        }
        return {};
    }

    uint16_t port() const { return port_; }

private:
    asio::ip::udp::socket socket_;
    uint16_t              port_ = 0;
};

void test_udp_receive_and_route() {
    PeerIo peer_io;

    Facade f;
    check(f.build(udp_config(0)), "udp: transport built on a kernel-chosen port");
    check(f.transport->start(), "udp: start() succeeds");
    check(f.rec.wait_link_ups(1),
          "udp: binding announces one link_up");

    const uint16_t port = port_from_udp_tag(f.first_link_up());
    check(port != 0, "udp: the link_up tag carries the bound port as udp:<port>");

    UdpPeer peer_a(peer_io.io());
    peer_a.send_to(FakeParser::frame(bytes("dgram")), port);
    check(f.rec.wait_frames(1),
          "udp: a datagram is reassembled into a frame");
    check(f.rec.wait_link_ups(2),
          "udp: the sender becomes a peer link");
    {
        const std::vector<Frame>      frames = f.rec.frames();
        const std::vector<std::string> ups   = f.rec.link_ups();
        const std::string want = "127.0.0.1:" + std::to_string(peer_a.port());
        check(frames.size() == 1 && sptest::as_string(frames[0].payload) == "dgram",
              "udp: the datagram payload is intact");
        check(frames.size() == 1 && frames[0].tag == want,
              "udp: the frame is tagged with the sender's ip:port");
        bool tagged_peer = false;
        for (const std::string& up : ups) {
            tagged_peer = tagged_peer || up == want;
        }
        check(tagged_peer, "udp: the peer link_up uses the sender's ip:port");
    }

    // Reply by tag: the frame goes back to the peer that sent.
    check(f.transport->send("127.0.0.1:" + std::to_string(peer_a.port()), bytes("reply")),
          "udp: send() to a peer tag returns true");
    check(peer_a.receive() == FakeParser::frame(bytes("reply")),
          "udp: the reply reaches the peer that sent");

    // Two peers: the reply has to go to the right one.
    UdpPeer peer_b(peer_io.io());
    peer_b.send_to(FakeParser::frame(bytes("from-b")), port);
    check(f.rec.wait_frames(2),
          "udp: a second peer's datagram is delivered");
    check(f.transport->broadcast(bytes("both")), "udp: broadcast() returns true with two peers");
    {
        const std::vector<uint8_t> want = FakeParser::frame(bytes("both"));
        check(peer_a.receive() == want, "udp: broadcast reaches the first peer");
        check(peer_b.receive() == want, "udp: broadcast reaches the second peer");
    }
}

void test_udp_send_to() {
    PeerIo peer_io;

    Facade f;
    check(f.build(udp_config(0)), "udp: transport built for send_to()");
    check(f.transport->start(), "udp: listening for send_to()");
    check(f.rec.wait_link_ups(1),
          "udp: bound for the send_to() case");

    UdpPeer target(peer_io.io());

    // Numeric address: the frame goes straight out.
    check(f.transport->send_to("127.0.0.1", target.port(), bytes("numeric")),
          "udp: send_to() with a numeric address returns true");
    check(target.receive() == FakeParser::frame(bytes("numeric")),
          "udp: send_to() with a numeric address reaches the target");

    // Host name: the facade resolves it asynchronously and then sends.
    const bool queued = f.transport->send_to("localhost", target.port(), bytes("named"));
    check(queued, "udp: send_to() with a host name returns true, resolution is async");
    check(target.receive() == FakeParser::frame(bytes("named")),
          "udp: send_to() with a host name reaches the target");

    // A destination that cannot be resolved is an error event, not a throw.
    f.rec.clear();
    f.transport->send_to("no-such-host.invalid", 9, bytes("nowhere"));
    check(f.rec.wait_error_code(kResolveFailed),
          "udp: an unresolvable host is reported with code -7");
}

void test_udp_max_links() {
    PeerIo peer_io;

    sp::TransportConfig cfg = udp_config(0);
    cfg.max_links           = 1;

    Facade f;
    check(f.build(cfg), "udp: transport built with max_links = 1");
    check(f.transport->start(), "udp: listening with max_links = 1");
    check(f.rec.wait_link_ups(1),
          "udp: bound for the max_links case");

    const uint16_t port = port_from_udp_tag(f.first_link_up());

    UdpPeer first(peer_io.io());
    first.send_to(FakeParser::frame(bytes("one")), port);
    check(f.rec.wait_frames(1),
          "udp: at max_links the first peer is still served");

    UdpPeer second(peer_io.io());
    second.send_to(FakeParser::frame(bytes("two")), port);
    check(f.rec.wait_error_code(kMaxLinks),
          "udp: a peer past max_links is refused with code -6");
}

void test_udp_large_datagram() {
    PeerIo peer_io;

    Facade f;
    check(f.build(udp_config(0)), "udp: transport built for the large datagram case");
    check(f.transport->start(), "udp: listening for the large datagram case");
    check(f.rec.wait_link_ups(1),
          "udp: bound for the large datagram case");

    const uint16_t             port = port_from_udp_tag(f.first_link_up());
    const std::vector<uint8_t> payload(1000, 0x33);

    UdpPeer peer(peer_io.io());
    peer.send_to(FakeParser::frame(payload), port);
    check(f.rec.wait_frames(1),
          "udp: a 1 KB payload in one datagram is delivered whole");
    {
        const std::vector<Frame> frames = f.rec.frames();
        check(frames.size() == 1 && frames[0].payload == payload,
              "udp: the 1 KB payload survives intact");
    }
}

// -------------------------------------------------
// UDP: a fixed destination configured ahead of time
// -------------------------------------------------
// peer_host / peer_port name a destination the transport registers on its own
// once it is bound, without waiting for that peer to send anything first.
void test_udp_fixed_peer_is_registered() {
    PeerIo peer_io;

    // The destination exists before the transport starts, so nothing but the
    // configuration can have registered it.
    UdpPeer target(peer_io.io());

    sp::TransportConfig cfg = udp_config(0);
    cfg.peer_host           = "127.0.0.1";
    cfg.peer_port           = target.port();

    Facade f;
    check(f.build(cfg), "udp fixed peer: transport built with peer_host/peer_port");
    check(f.transport->start(), "udp fixed peer: start() succeeds");

    const std::string want = "127.0.0.1:" + std::to_string(target.port());
    check(f.rec.wait_link_ups(2),
          "udp fixed peer: the configured destination is announced as a link");
    {
        const std::vector<std::string> ups = f.rec.link_ups();
        bool saw_socket = false;
        bool saw_peer   = false;
        for (const std::string& tag : ups) {
            saw_socket = saw_socket || port_from_udp_tag(tag) != 0;
            saw_peer   = saw_peer || tag == want;
        }
        check(saw_socket, "udp fixed peer: the socket itself still announces link_up");
        check(saw_peer, "udp fixed peer: the peer is tagged with its ip:port");
    }
    // Everything on_link_up announced has to be findable through links(): the
    // socket's own tag included, even though nothing is sent to it.
    {
        const std::vector<std::string> tags = f.transport->links();
        bool has_peer = false;
        for (const std::string& tag : tags) {
            has_peer = has_peer || tag == want;
        }
        check(has_peer, "udp fixed peer: links() lists the configured destination");
        const std::vector<std::string> ups = f.rec.link_ups();
        check(tags.size() == ups.size(),
              "udp fixed peer: links() reports exactly what on_link_up announced");
    }

    // The socket's own tag is listed but is not a destination: a send to it is
    // refused, and it does not make broadcast() report a failed write.
    {
        std::string self_tag;
        for (const std::string& tag : f.rec.link_ups()) {
            if (port_from_udp_tag(tag) != 0) {
                self_tag = tag;
            }
        }
        check(!self_tag.empty(), "udp fixed peer: the socket's own tag was announced");
        check(!f.transport->send(self_tag, bytes("nowhere")),
              "udp fixed peer: send() to the socket itself returns false");
    }

    // The configured peer is usable without it having sent anything.
    check(f.transport->send(want, bytes("hello-fixed")),
          "udp fixed peer: send() to the configured tag returns true");
    check(target.receive() == FakeParser::frame(bytes("hello-fixed")),
          "udp fixed peer: the configured destination receives the frame");
    check(f.rec.wait_sent(1), "udp fixed peer: on_sent fires for it");

    check(f.transport->broadcast(bytes("to-all")),
          "udp fixed peer: broadcast() returns true");
    check(target.receive() == FakeParser::frame(bytes("to-all")),
          "udp fixed peer: broadcast reaches the configured destination");
    sleep_ms(100);
    check(f.rec.error_count() == 0,
          "udp fixed peer: the socket link produces no write error");
}

void test_udp_fixed_peer_host_name() {
    PeerIo peer_io;

    UdpPeer target(peer_io.io());

    // A host name goes through the asynchronous resolver, not make_address.
    sp::TransportConfig cfg = udp_config(0);
    cfg.peer_host           = "localhost";
    cfg.peer_port           = target.port();

    Facade f;
    check(f.build(cfg), "udp fixed peer: transport built with a host name");
    check(f.transport->start(), "udp fixed peer: start() succeeds with a host name");

    const std::string want = "127.0.0.1:" + std::to_string(target.port());
    check(f.rec.wait_link_ups(2),
          "udp fixed peer: the host name is resolved and registered");
    check(f.transport->send(want, bytes("named")),
          "udp fixed peer: send() to the resolved tag returns true");
    check(target.receive() == FakeParser::frame(bytes("named")),
          "udp fixed peer: the resolved destination receives the frame");
}

void test_udp_unresolvable_fixed_peer() {
    PeerIo peer_io;

    sp::TransportConfig cfg = udp_config(0);
    cfg.peer_host           = "no-such-host.invalid";
    cfg.peer_port           = 9;

    Facade f;
    check(f.build(cfg), "udp fixed peer: transport built with an unresolvable host");
    check(f.transport->start(),
          "udp fixed peer: an unresolvable destination does not make start() fail");
    check(f.rec.wait_error_code(kResolveFailed),
          "udp fixed peer: the unresolvable destination is reported with code -7");
    check(f.rec.wait_link_ups(1), "udp fixed peer: the socket itself is still up");
    check(f.transport->links().size() == 1,
          "udp fixed peer: no peer link is added for a failed lookup");
}

void test_udp_incomplete_fixed_peer_is_ignored() {
    PeerIo peer_io;

    // A host without a port, or a port without a host, is not a destination:
    // both have to be set before the transport registers anything.
    {
        sp::TransportConfig cfg = udp_config(0);
        cfg.peer_host           = "127.0.0.1";   // port left at 0

        Facade f;
        check(f.build(cfg), "udp fixed peer: transport built with a host and no port");
        check(f.transport->start(), "udp fixed peer: host-without-port still starts");
        check(f.rec.wait_link_ups(1), "udp fixed peer: the socket announces itself");
        sleep_ms(200);
        check(f.rec.link_up_count() == 1,
              "udp fixed peer: a host without a port registers no peer");
        check(f.transport->links().size() == 1,
              "udp fixed peer: links() has the socket and nothing else");
    }
    {
        sp::TransportConfig cfg = udp_config(0);
        cfg.peer_port           = 9;             // host left empty

        Facade f;
        check(f.build(cfg), "udp fixed peer: transport built with a port and no host");
        check(f.transport->start(), "udp fixed peer: port-without-host still starts");
        check(f.rec.wait_link_ups(1), "udp fixed peer: the socket announces itself");
        sleep_ms(200);
        check(f.rec.link_up_count() == 1,
              "udp fixed peer: a port without a host registers no peer");
    }
}

}  // namespace

int main() {
    std::cout << "== factory and lifecycle ==" << std::endl;
    test_factory_requires_parser();
    test_lifecycle_is_idempotent();

    std::cout << "== tcp_server ==" << std::endl;
    test_tcp_server_receive_shapes();
    test_tcp_server_frame_timeout();
    test_tcp_server_send_paths();
    test_send_budget();
    test_parser_refusal_is_reported();
    test_tcp_server_max_links();
    test_tcp_server_link_down_once();
    test_tcp_server_start_failure();
    test_stop_from_callback_does_not_hang();
    test_tcp_server_back_pressure();

    std::cout << "== tcp_client ==" << std::endl;
    test_tcp_client_roundtrip();
    test_tcp_client_disconnect_detected();
    test_tcp_client_reconnects();
    test_tcp_client_no_reconnect();
    test_tcp_client_idle_timeout();
    test_tcp_client_config_errors();

    std::cout << "== udp ==" << std::endl;
    test_udp_receive_and_route();
    test_udp_send_to();
    test_udp_max_links();
    test_udp_large_datagram();
    test_udp_fixed_peer_is_registered();
    test_udp_fixed_peer_host_name();
    test_udp_unresolvable_fixed_peer();
    test_udp_incomplete_fixed_peer_is_ignored();

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all checks passed" << std::endl;
    return 0;
}
