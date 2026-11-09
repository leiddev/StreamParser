/*
 * TransportBase.cc - io thread, link table, write ordering and event dispatch.
 *
 * Leid 2026.10
 */
#include "TransportImpl.h"

#include <exception>
#include <string>
#include <utility>

#include <sp/StreamParserLog.h>

namespace sp {
namespace detail {

namespace {
constexpr int kWriteFailed = 1;   // on_error code: the OS refused a frame

/* A refused frame is reported by the parser that refused it, but only the link
   parsers have that callback wired; the encoder does not. State the sizes the
   transport knows here, so an oversized payload is not just "cannot encode".
   The framed result, not the payload, is what has to fit the budget, and the
   framing costs protocol-defined bytes the transport cannot count: say that,
   instead of putting the two numbers side by side where "payload 10240,
   max_frame_bytes 10240" would read like a contradiction. */
std::string encode_error_detail(std::size_t payload_size, std::size_t frame_limit) {
    return "cannot encode a " + std::to_string(payload_size) +
           "-byte payload: the framed result exceeds max_frame_bytes (" +
           std::to_string(frame_limit) + ")";
}
}  // namespace

TransportBase::TransportBase(TransportConfig cfg, ParserFactory factory, TransportHandler handler)
    : cfg_(std::move(cfg)), factory_(std::move(factory)), handler_(std::move(handler)) {
    /* CreateTransport() already rejects an empty factory; keep the encoder null
       when that guarantee is bypassed so start() can report it. */
    const std::string encoder_tag = cfg_.name.empty() ? "encoder" : cfg_.name + "#encoder";
    encoder_ = createParser(encoder_tag);
}

TransportBase::~TransportBase() {
    stop();
}

// -------------------------------------------------
// Lifecycle
// -------------------------------------------------
bool TransportBase::start() {
    if (running_.load()) {
        return true;
    }
    if (!encoder_) {
        emitError(cfg_.name, -1, "parser factory returned no parser for the encoder");
        return false;
    }

    stopping_ = false;
    io_.restart();
    guard_.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
        asio::make_work_guard(io_)));
    thread_ = std::thread([this]() { runIo(); });
    io_thread_id_ = thread_.get_id();
    running_ = true;

    /* open() creates asio objects and posts the first accept/connect/receive,
       so it has to run on the io thread. The caller waits for the result so a
       bind failure still surfaces as start() == false. */
    std::promise<bool> promise;
    std::future<bool>  future = promise.get_future();
    post([this, &promise]() {
        bool ok = false;
        try {
            ok = open();
        } catch (const std::exception& e) {
            emitError(cfg_.name, -1, std::string("open failed: ") + e.what());
            ok = false;
        } catch (...) {
            emitError(cfg_.name, -1, "open failed: unknown exception");
            ok = false;
        }
        promise.set_value(ok);
    });

    if (!future.get()) {
        stop();
        return false;
    }
    return true;
}

void TransportBase::stop() {
    if (!thread_.joinable()) {
        running_ = false;
        return;
    }
    if (std::this_thread::get_id() == io_thread_id_) {
        /* stop() joins the io thread, so it cannot be called from a callback.
           Say so loudly instead of deadlocking the host. */
        SP_LOG_ERROR("transport", (cfg_.name.empty() ? "transport" : cfg_.name)
                                      << ": stop() called from the io thread, call it from "
                                         "another thread (or the shutdown hangs)");
        return;
    }

    stopping_ = true;
    if (guard_) {
        guard_->reset();
    }

    std::promise<void> promise;
    std::future<void>  future = promise.get_future();
    post([this, &promise]() {
        shutdownOnIo();
        promise.set_value();
    });
    future.wait();

    thread_.join();
    running_ = false;
    io_thread_id_ = std::thread::id{};
    if (cfg_.name.empty()) {
        SP_LOG_DEBUG("transport", "stopped");
    } else {
        SP_LOG_INFO("transport", cfg_.name << ": stopped");
    }
}

bool TransportBase::is_running() const {
    return running_.load();
}

void TransportBase::runIo() {
    for (;;) {
        try {
            io_.run();
            break;
        } catch (const std::exception& e) {
            /* A throwing handler must not take the whole transport down. */
            emitError(cfg_.name, -1, std::string("io thread: ") + e.what());
        } catch (...) {
            emitError(cfg_.name, -1, "io thread: unknown exception");
        }
    }
    running_ = false;
}

void TransportBase::shutdownOnIo() {
    try {
        close();
    } catch (const std::exception& e) {
        emitError(cfg_.name, -1, std::string("close failed: ") + e.what());
    } catch (...) {
        emitError(cfg_.name, -1, "close failed: unknown exception");
    }

    std::vector<LinkPtr> all;
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        for (auto& kv : links_) {
            kv.second->up = false;
            kv.second->tx.clear();
            all.push_back(kv.second);
        }
        links_.clear();
    }
}

// -------------------------------------------------
// Send path
// -------------------------------------------------
TransportBase::Frame TransportBase::encode(const std::vector<uint8_t>& payload) {
    if (!encoder_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(encode_mutex_);
    std::vector<uint8_t> frame = encoder_->buildFrame(payload);
    if (frame.empty()) {
        return nullptr;
    }
    return std::make_shared<const std::vector<uint8_t>>(std::move(frame));
}

bool TransportBase::send(const std::string& tag, const std::vector<uint8_t>& payload) {
    if (!running_.load() || stopping_.load()) {
        return false;
    }

    Frame frame = encode(payload);
    if (!frame) {
        emitError(tag, -2, encode_error_detail(payload.size(), cfg_.max_frame_bytes));
        return false;
    }

    LinkPtr link;
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        auto it = links_.find(tag);
        if (it == links_.end()) {
            return false;
        }
        if (it->second->tx.size() >= cfg_.max_pending_frames) {
            SP_LOG_WARN("transport", "link " << tag << ": send queue full, frame dropped");
            return false;
        }
        it->second->tx.push_back(frame);
        link = it->second;
    }

    post([this, link]() { flush(link); });
    return true;
}

bool TransportBase::broadcast(const std::vector<uint8_t>& payload) {
    if (!running_.load() || stopping_.load()) {
        return false;
    }

    Frame frame = encode(payload);
    if (!frame) {
        emitError(cfg_.name, -2, encode_error_detail(payload.size(), cfg_.max_frame_bytes));
        return false;
    }

    std::vector<LinkPtr> targets;
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        for (auto& kv : links_) {
            if (kv.second->tx.size() >= cfg_.max_pending_frames) {
                SP_LOG_WARN("transport", "link " << kv.first << ": send queue full, frame dropped");
                continue;
            }
            kv.second->tx.push_back(frame);
            targets.push_back(kv.second);
        }
    }
    if (targets.empty()) {
        return false;
    }
    for (auto& link : targets) {
        post([this, link]() { flush(link); });
    }
    return true;
}

bool TransportBase::send_to(const std::string& host,
                            uint16_t           port,
                            const std::vector<uint8_t>& payload) {
    if (!running_.load() || stopping_.load()) {
        return false;
    }
    Frame frame = encode(payload);
    if (!frame) {
        emitError(cfg_.name, -2, encode_error_detail(payload.size(), cfg_.max_frame_bytes));
        return false;
    }
    return doSendTo(host, port, frame);
}

void TransportBase::pushAndFlush(const LinkPtr& link, const Frame& frame) {
    if (!link) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        if (link->tx.size() >= cfg_.max_pending_frames) {
            SP_LOG_WARN("transport", "link " << link->tag << ": send queue full, frame dropped");
            return;
        }
        link->tx.push_back(frame);
    }
    post([this, link]() { flush(link); });
}

void TransportBase::flush(const LinkPtr& link) {
    if (!link) {
        return;
    }

    Frame  frame;
    Writer writer;
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        if (link->writing || link->tx.empty()) {
            return;
        }
        link->writing = true;
        frame = link->tx.front();
        link->tx.pop_front();
        writer = link->writer;
    }

    if (!writer) {
        std::lock_guard<std::mutex> lock(link_mutex_);
        link->writing = false;
        return;
    }

    writer(frame, [this, link, frame](bool ok, const std::string& detail) {
        {
            std::lock_guard<std::mutex> lock(link_mutex_);
            link->writing = false;
        }
        if (stopping_.load()) {
            return;
        }
        if (ok) {
            emitSent(link->tag, frame);
        } else {
            emitError(link->tag, kWriteFailed, detail.empty() ? std::string("write failed") : detail);
            /* Take the link out of the table *before* announcing the failure: the
               owner of the removal owns the link-down event, so the socket error
               that follows reports nothing a second time. Stop here as well - the
               remaining frames belong to a peer that is already gone. */
            if (removeLink(link->tag)) {
                emitLinkDown(link->tag, "write failed");
            }
            return;
        }
        /* Continue with the next frame from the io thread; going through post()
           keeps a synchronously-completing writer from recursing. */
        post([this, link]() { flush(link); });
    });
}

// -------------------------------------------------
// Link table
// -------------------------------------------------
TransportBase::LinkPtr TransportBase::addLink(const std::string& tag, Writer writer) {
    auto link = std::make_shared<Link>();
    link->tag = tag;
    link->writer = std::move(writer);
    {
        std::lock_guard<std::mutex> lock(link_mutex_);
        links_[tag] = link;
    }
    return link;
}

bool TransportBase::removeLink(const std::string& tag) {
    std::lock_guard<std::mutex> lock(link_mutex_);
    return links_.erase(tag) > 0;
}

void TransportBase::markUp(const std::string& tag, bool up) {
    std::lock_guard<std::mutex> lock(link_mutex_);
    auto it = links_.find(tag);
    if (it != links_.end()) {
        it->second->up = up;
    }
}

TransportBase::LinkPtr TransportBase::findLink(const std::string& tag) const {
    std::lock_guard<std::mutex> lock(link_mutex_);
    auto it = links_.find(tag);
    return it == links_.end() ? LinkPtr() : it->second;
}

std::vector<TransportBase::LinkPtr> TransportBase::snapshot() const {
    std::vector<LinkPtr> all;
    std::lock_guard<std::mutex> lock(link_mutex_);
    all.reserve(links_.size());
    for (auto& kv : links_) {
        all.push_back(kv.second);
    }
    return all;
}

std::size_t TransportBase::linkCount() const {
    std::lock_guard<std::mutex> lock(link_mutex_);
    return links_.size();
}

bool TransportBase::atCapacity() const {
    return cfg_.max_links != 0 && linkCount() >= cfg_.max_links;
}

std::vector<std::string> TransportBase::links() const {
    std::vector<std::string> tags;
    std::lock_guard<std::mutex> lock(link_mutex_);
    for (auto& kv : links_) {
        if (kv.second->up) {
            tags.push_back(kv.first);
        }
    }
    return tags;
}

// -------------------------------------------------
// Parser plumbing
// -------------------------------------------------
std::unique_ptr<StreamParser> TransportBase::createParser(const std::string& tag) {
    return factory_ ? factory_(tag) : nullptr;
}

void TransportBase::wireParser(StreamParser* parser, std::function<std::string()> tag_of) {
    if (!parser) {
        return;
    }
    parser->setFrameCallback(
        [this, tag_of](const std::string&, const uint8_t* data, std::size_t len) {
            emitFrame(tag_of(), data, len);
        });
    parser->setErrorCallback(
        [this, tag_of](const std::string&, StreamParser::ErrorCode code, const std::string& detail) {
            emitError(tag_of(), static_cast<int>(code), detail);
        });
}

std::unique_ptr<StreamParser> TransportBase::makeParser(const std::string& tag) {
    auto parser = createParser(tag);
    wireParser(parser.get(), [tag]() { return tag; });
    return parser;
}

// -------------------------------------------------
// Events
// -------------------------------------------------
void TransportBase::emitFrame(const std::string& tag, const uint8_t* data, std::size_t len) {
    if (stopping_.load() || !handler_.on_frame) {
        return;
    }
    handler_.on_frame(tag, data, len);
}

void TransportBase::emitLinkUp(const std::string& tag) {
    if (stopping_.load()) {
        return;
    }
    markUp(tag, true);
    if (handler_.on_link_up) {
        handler_.on_link_up(tag);
    }
}

void TransportBase::emitLinkDown(const std::string& tag, const std::string& why) {
    if (stopping_.load()) {
        return;
    }
    markUp(tag, false);
    if (handler_.on_link_down) {
        handler_.on_link_down(tag, why);
    }
}

void TransportBase::emitError(const std::string& tag, int code, const std::string& detail) {
    if (stopping_.load()) {
        return;
    }
    if (handler_.on_error) {
        handler_.on_error(tag, code, detail);
    }
}

void TransportBase::emitSent(const std::string& tag, const Frame& frame) {
    if (stopping_.load() || !handler_.on_sent || !frame) {
        return;
    }
    handler_.on_sent(tag, *frame);
}

// -------------------------------------------------
// Helpers
// -------------------------------------------------
void TransportBase::post(std::function<void()> fn) {
    asio::post(io_, std::move(fn));
}

void TransportBase::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    auto timer = std::make_shared<asio::steady_timer>(io_, delay);
    timer->async_wait([timer, fn](const asio::error_code& ec) {
        if (!ec && fn) {
            fn();
        }
    });
}

std::string TransportBase::endpointTag(const asio::ip::tcp::endpoint& ep) {
    return ep.address().to_string() + ":" + std::to_string(ep.port());
}

std::string TransportBase::endpointTag(const asio::ip::udp::endpoint& ep) {
    return ep.address().to_string() + ":" + std::to_string(ep.port());
}

}  // namespace detail
}  // namespace sp
