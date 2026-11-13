/*
 * TransportImpl.h - Shared skeleton for the four Transport implementations.
 *
 * Leid 2026.10
 *
 * Internal header: it is only included by the src/Transport*.cc files, so it
 * is allowed to depend on asio. Every bit of plumbing that the four kinds have
 * in common lives here (io_context + worker thread, link table, per-link write
 * queue and ordering, parser factory, event dispatch); a concrete transport
 * only says how a link is opened, closed and written.
 */
#pragma once
#ifndef _SP_TRANSPORT_IMPL_H_
#define _SP_TRANSPORT_IMPL_H_

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sp/Transport.h>

namespace sp {
namespace detail {

class TransportBase : public Transport {
public:
    TransportBase(TransportConfig cfg, ParserFactory factory, TransportHandler handler);
    ~TransportBase() override;

    bool start() override;
    void stop() override;
    bool is_running() const override;
    bool send(const std::string& tag, const std::vector<uint8_t>& payload) override;
    bool broadcast(const std::vector<uint8_t>& payload) override;
    bool send_to(const std::string& host,
                 uint16_t           port,
                 const std::vector<uint8_t>& payload) override;
    std::vector<std::string> links() const override;

protected:
    using Frame     = std::shared_ptr<const std::vector<uint8_t>>;
    using WriteDone = std::function<void(bool ok, const std::string& detail)>;
    using Writer    = std::function<void(const Frame& frame, WriteDone done)>;

    // One logical link: a device, an accepted client, a server connection or
    // a UDP peer. The writer is set once, when the link is created.
    //
    // A link with `writable == false` is listed and announced like any other,
    // but nothing can be sent to it: it names something the transport reports
    // on rather than a destination. The UDP socket's own tag is the one such
    // link today. broadcast() skips them and send() refuses them, instead of
    // recording an error for a write that could never succeed.
    struct Link {
        std::string       tag;
        Writer            writer;
        std::deque<Frame> tx;         // encoded frames waiting for the OS
        bool              writing  = false;
        bool              up       = false;
        bool              writable = true;
    };
    using LinkPtr = std::shared_ptr<Link>;

    // ---- hooks implemented by the concrete transport ----
    // All of them run on the io thread.
    virtual bool open()  = 0;   // bind / listen / connect / open the device
    virtual void close() = 0;   // close the listener and every link
    virtual bool doSendTo(const std::string& host, uint16_t port, const Frame& frame) {
        (void)host;
        (void)port;
        (void)frame;
        return false;           // only Udp supports explicit destinations
    }

    asio::io_context&      io()           { return io_; }
    const TransportConfig& config() const { return cfg_; }
    bool                   stopping() const { return stopping_.load(); }

    // ---- parser plumbing ----
    // The transport builds the parser and hands it to an adapter; the base
    // wires the callbacks back into the event helpers.
    std::unique_ptr<StreamParser> createParser(const std::string& tag);
    void wireParser(StreamParser* parser, std::function<std::string()> tag_of);
    std::unique_ptr<StreamParser> makeParser(const std::string& tag);

    // ---- link table ----
    LinkPtr              addLink(const std::string& tag, Writer writer);
    // Adds a link that is reported (links(), on_link_up) but cannot be written
    // to: it names the transport itself rather than a destination.
    LinkPtr              addSinkLink(const std::string& tag);
    // Removes the link and reports whether it was still present: the caller that
    // gets true owns the link-down event, so a write failure followed by the
    // socket error that reports the same disconnect does not notify the host twice.
    bool                 removeLink(const std::string& tag);
    void                 markUp(const std::string& tag, bool up);
    LinkPtr              findLink(const std::string& tag) const;
    std::vector<LinkPtr> snapshot() const;
    std::size_t          linkCount() const;
    bool                 atCapacity() const;

    // ---- events (all dispatched on the io thread) ----
    void emitFrame(const std::string& tag, const uint8_t* data, std::size_t len);
    void emitLinkUp(const std::string& tag);
    void emitLinkDown(const std::string& tag, const std::string& why);
    void emitError(const std::string& tag, int code, const std::string& detail);
    void emitSent(const std::string& tag, const Frame& frame);

    // ---- helpers ----
    Frame       encode(const std::vector<uint8_t>& payload);
    void        post(std::function<void()> fn);
    void        after(std::chrono::milliseconds delay, std::function<void()> fn);
    void        pushAndFlush(const LinkPtr& link, const Frame& frame);
    static std::string endpointTag(const asio::ip::tcp::endpoint& ep);
    static std::string endpointTag(const asio::ip::udp::endpoint& ep);

private:
    void runIo();
    void flush(const LinkPtr& link);
    void shutdownOnIo();
    // Takes the io thread down without joining it. Runs on the io thread, where
    // stop() cannot: a failed start() is discovered there.
    void stopFromIo();
    void joinIoThread();

    TransportConfig  cfg_;
    ParserFactory    factory_;
    TransportHandler handler_;
    std::unique_ptr<StreamParser> encoder_;   // framing only, never fed

    asio::io_context io_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
    std::thread      thread_;
    std::thread::id  io_thread_id_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex                       link_mutex_;
    std::unordered_map<std::string, LinkPtr> links_;
    std::mutex                               encode_mutex_;
};

}  // namespace detail

// Concrete makers, one per file. CreateTransport() picks between them.
std::unique_ptr<Transport> MakeSerialTransport(TransportConfig cfg, ParserFactory factory,
                                               TransportHandler handler);
std::unique_ptr<Transport> MakeTcpServerTransport(TransportConfig cfg, ParserFactory factory,
                                                  TransportHandler handler);
std::unique_ptr<Transport> MakeTcpClientTransport(TransportConfig cfg, ParserFactory factory,
                                                  TransportHandler handler);
std::unique_ptr<Transport> MakeUdpTransport(TransportConfig cfg, ParserFactory factory,
                                            TransportHandler handler);

}  // namespace sp

#endif  // !_SP_TRANSPORT_IMPL_H_
