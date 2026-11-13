/*
 * keepalive and log_link_events for the Transport facade.
 *
 * Both are configuration that produces no callback event, so neither shows up
 * in an on_error counter. They are checked where the effect is observable:
 *
 *  - keepalive is a per-socket option, so the test hands the adapter a socket
 *    it built itself and reads the kernel's view of it back (SO_KEEPALIVE,
 *    TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT), then checks that keeping it on
 *    does not disturb traffic through the facade. That read is Linux only:
 *    the Windows equivalent (SIO_KEEPALIVE_VALS) has no query form, so there
 *    the group is skipped rather than checked against a guess.
 *
 *  - log_link_events is checked against the library's own log sink: with the
 *    flag on, connecting and disconnecting print a line; with it off they do
 *    not, while the callbacks fire either way.
 */
#include <asio.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/TransportTestKit.h"
#include <sp/StreamParserLog.h>
#include <sp/TcpSessionParserAdapter.h>
#include <sp/TcpTypes.h>

#if defined(__linux__)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using sptest::bytes;
using sptest::FakeParser;
using sptest::Frame;
using sptest::Recorder;
using sptest::kWaitMs;

int g_failures = 0;
int g_skipped  = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "[OK]   " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

// Only the groups that cannot run on this platform use it.
#if !defined(__linux__)
void skip(const char* what) {
    std::cout << "[SKIP] " << what << std::endl;
    ++g_skipped;
}
#endif

// -------------------------------------------------
// Captures the library's own log records for as long as it lives
// -------------------------------------------------
class LogCapture {
public:
    LogCapture() {
        previous_callback_ = splog::callback();
        previous_level_    = splog::level();
        splog::set_callback([this](const splog::Record& r) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                records_.push_back(Line{r.level, r.component ? r.component : "", r.message});
                ++total_;
            }
            cv_.notify_all();
        });
        splog::set_level(splog::Level::trace);
        splog::set_stderr_fallback(false);
    }

    ~LogCapture() {
        splog::set_callback(previous_callback_);
        splog::set_level(previous_level_);
        splog::set_stderr_fallback(true);   // no getter; true is the library default
    }

    struct Line {
        splog::Level level;
        std::string  component;
        std::string  message;
    };

    std::vector<Line> lines() {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    std::size_t total() {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

    // Waits until at least n records have been captured.
    bool wait_for(std::size_t n, int timeout_ms = kWaitMs) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::mutex> lock(mutex_);
        while (total_ < n) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return total_ >= n;
            }
        }
        return true;
    }

    // How many captured records contain `needle`.
    std::size_t mentioning(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const Line& line : records_) {
            if (line.message.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.clear();
        total_ = 0;
    }

private:
    splog::Callback          previous_callback_;
    splog::Level             previous_level_    = splog::Level::info;
    mutable std::mutex       mutex_;
    std::condition_variable  cv_;
    std::vector<Line>        records_;
    std::size_t              total_ = 0;
};

// The io thread the bare peers run on, for the callbacks a facade needs.
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

class TcpPeer {
public:
    asio::error_code connect(asio::io_context& io, uint16_t port) {
        asio::error_code ec;
        socket_.reset(new asio::ip::tcp::socket(io));
        socket_->connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
        return ec;
    }
    void close() {
        if (!socket_) {
            return;
        }
        asio::error_code ec;
        socket_->close(ec);
        socket_.reset();
    }
    void write(const std::vector<uint8_t>& data) {
        if (!socket_) {
            return;
        }
        asio::error_code ec;
        asio::write(*socket_, asio::buffer(data), ec);
    }

private:
    std::shared_ptr<asio::ip::tcp::socket> socket_;
};

uint16_t free_tcp_port(asio::io_context& io) {
    asio::ip::tcp::acceptor probe(io,
                                  asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const uint16_t port = probe.local_endpoint().port();
    asio::error_code ec;
    probe.close(ec);
    return port;
}

sp::TransportConfig tcp_server_config(uint16_t port) {
    sp::TransportConfig cfg;
    cfg.kind         = sp::TransportKind::TcpServer;
    cfg.name         = "ka";
    cfg.bind_address = "127.0.0.1";
    cfg.port         = port;
    return cfg;
}

struct Facade {
    Recorder                       rec;
    std::unique_ptr<sp::Transport> transport;

    ~Facade() {
        if (transport) {
            transport->stop();
        }
    }

    bool build(const sp::TransportConfig& cfg) {
        transport = sp::CreateTransport(
            cfg, sptest::fake_factory(cfg.frame_timeout_ms, cfg.max_frame_bytes), rec.handler());
        return transport != nullptr;
    }
};

// -------------------------------------------------
// keepalive: what the kernel thinks of the adapter's socket
// -------------------------------------------------
#if defined(__linux__)

int read_int_option(int fd, int level, int name) {
    int       value = -1;
    socklen_t len   = sizeof(value);
    if (::getsockopt(fd, level, name, &value, &len) != 0) {
        return -1;
    }
    return value;
}

// Builds a connected pair, hands one end to an adapter over a dup()ed
// descriptor, and keeps the original to read options from. Both descriptors
// refer to the same socket, so the options the adapter sets are the ones read.
struct AdapterProbe {
    asio::io_context              io;
    asio::ip::tcp::acceptor       acceptor{io};
    asio::ip::tcp::socket         client{io};
    asio::ip::tcp::socket         server{io};
    int                           probe_fd = -1;
    std::shared_ptr<sp::TcpSessionParserAdapter> adapter;

    bool connect() {
        asio::error_code ec;
        acceptor.open(asio::ip::tcp::v4(), ec);
        if (ec) {
            return false;
        }
        acceptor.bind(
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), ec);
        if (ec) {
            return false;
        }
        acceptor.listen(1, ec);
        if (ec) {
            return false;
        }
        const uint16_t port = acceptor.local_endpoint(ec).port();
        client.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
        if (ec) {
            return false;
        }
        acceptor.accept(server, ec);
        return !ec;
    }

    bool start(const sp::TcpAdapterConfig& cfg, const std::string& tag) {
        probe_fd = ::dup(server.native_handle());
        if (probe_fd < 0) {
            return false;
        }
        asio::ip::tcp::socket adopted(io);
        adopted.assign(asio::ip::tcp::v4(), ::dup(server.native_handle()));
        adapter = std::make_shared<sp::TcpSessionParserAdapter>(
            io, std::move(adopted), tag,
            std::unique_ptr<sp::StreamParser>(new FakeParser(tag, 300, 4096)), cfg);
        adapter->start();
        return true;
    }

    ~AdapterProbe() {
        if (adapter) {
            adapter->close();
        }
        if (probe_fd >= 0) {
            ::close(probe_fd);
        }
    }
};

void test_keepalive_options_are_applied() {
    AdapterProbe probe;
    check(probe.connect(), "keepalive: a loopback socket pair is connected");

    sp::TcpAdapterConfig cfg;
    cfg.enable_keepalive           = true;
    cfg.keepalive_idle_seconds     = 11;
    cfg.keepalive_interval_seconds = 4;
    cfg.keepalive_probes           = 6;
    check(probe.start(cfg, "ka"), "keepalive: the adapter takes over one end");

    check(read_int_option(probe.probe_fd, SOL_SOCKET, SO_KEEPALIVE) == 1,
          "keepalive: SO_KEEPALIVE is on for the adapter socket");
    check(read_int_option(probe.probe_fd, SOL_TCP, TCP_KEEPIDLE) == 11,
          "keepalive: keepalive_idle_seconds reaches TCP_KEEPIDLE");
    check(read_int_option(probe.probe_fd, SOL_TCP, TCP_KEEPINTVL) == 4,
          "keepalive: keepalive_interval_seconds reaches TCP_KEEPINTVL");
    check(read_int_option(probe.probe_fd, SOL_TCP, TCP_KEEPCNT) == 6,
          "keepalive: keepalive_probes reaches TCP_KEEPCNT");
}

void test_keepalive_off_leaves_the_default() {
    AdapterProbe probe;
    check(probe.connect(), "keepalive off: a loopback socket pair is connected");

    sp::TcpAdapterConfig cfg;
    cfg.enable_keepalive = false;
    check(probe.start(cfg, "noka"), "keepalive off: the adapter takes over one end");

    check(read_int_option(probe.probe_fd, SOL_SOCKET, SO_KEEPALIVE) == 0,
          "keepalive off: SO_KEEPALIVE stays off");
}

#else

void test_keepalive_options_are_applied() {
    skip("keepalive option readback is Linux only (SIO_KEEPALIVE_VALS has no query form)");
}

void test_keepalive_off_leaves_the_default() {
    skip("keepalive option readback is Linux only");
}

#endif

// -------------------------------------------------
// keepalive through the facade: traffic keeps working
// -------------------------------------------------
void test_keepalive_does_not_break_traffic() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.keep_alive                 = true;
    cfg.keepalive_idle_seconds     = 10;
    cfg.keepalive_interval_seconds = 2;
    cfg.keepalive_probes           = 3;

    Facade f;
    check(f.build(cfg), "keepalive facade: transport built with keepalive on");
    check(f.transport->start(), "keepalive facade: listening with keepalive on");

    TcpPeer peer;
    check(peer.connect(peer_io.io(), port) == asio::error_code(),
          "keepalive facade: a client connects to the keepalive listener");
    check(f.rec.wait_link_ups(1), "keepalive facade: the link comes up");

    peer.write(FakeParser::frame(bytes("still-works")));
    check(f.rec.wait_frames(1), "keepalive facade: frames still arrive with keepalive on");
    {
        const std::vector<Frame> frames = f.rec.frames();
        check(frames.size() == 1 && sptest::as_string(frames[0].payload) == "still-works",
              "keepalive facade: the payload is intact");
    }
}

// -------------------------------------------------
// log_link_events
// -------------------------------------------------
void test_log_link_events_on() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    LogCapture log;

    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.name                = "linklog";
    cfg.log_link_events     = true;

    Facade f;
    check(f.build(cfg), "log on: transport built with log_link_events = true");
    check(f.transport->start(), "log on: listening");

    TcpPeer peer;
    peer.connect(peer_io.io(), port);
    check(f.rec.wait_link_ups(1), "log on: the link comes up");

    check(log.mentioning("connected") >= 1,
          "log on: connecting produces a 'connected' record");
    check(log.mentioning("linklog") >= 1,
          "log on: the record is prefixed with the transport name");

    peer.close();
    check(f.rec.wait_link_downs(1), "log on: the link goes down");
    check(log.mentioning("disconnected") >= 1,
          "log on: disconnecting logs its own line");
}

void test_log_link_events_off() {
    PeerIo peer_io;
    const uint16_t port = free_tcp_port(peer_io.io());

    LogCapture log;

    sp::TransportConfig cfg = tcp_server_config(port);
    cfg.name                = "linklog";
    cfg.log_link_events     = false;

    Facade f;
    check(f.build(cfg), "log off: transport built with log_link_events = false");
    check(f.transport->start(), "log off: listening");

    TcpPeer peer;
    peer.connect(peer_io.io(), port);
    check(f.rec.wait_link_ups(1),
          "log off: the link still comes up, the callback is unaffected");

    peer.close();
    check(f.rec.wait_link_downs(1), "log off: the link-down callback still fires");

    // Startup and error logs are outside the flag, so they may still appear;
    // the per-connection lines must not.
    check(log.mentioning("connected") == 0,
          "log off: no 'connected' line is printed");
    check(log.mentioning("disconnected") == 0,
          "log off: no 'disconnected' line is printed");
}

}  // namespace

int main() {
    std::cout << "== keepalive ==" << std::endl;
    test_keepalive_options_are_applied();
    test_keepalive_off_leaves_the_default();
    test_keepalive_does_not_break_traffic();

    std::cout << "== log_link_events ==" << std::endl;
    test_log_link_events_on();
    test_log_link_events_off();

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    if (g_skipped != 0) {
        std::cout << "all checks passed (" << g_skipped << " group(s) skipped)" << std::endl;
    } else {
        std::cout << "all checks passed" << std::endl;
    }
    return 0;
}
