/*
 * FakeTransport.h - In-memory Transport for tests.
 *
 * Leid 2026.10
 *
 * Business objects can be exercised without sockets, threads or timing: add
 * the links you want, inject the frames the device would have sent, then look
 * at what was written. No asio dependency, header only.
 */
#pragma once
#ifndef _SP_FAKE_TRANSPORT_H_
#define _SP_FAKE_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <sp/Transport.h>

namespace sp {

class FakeTransport : public Transport {
public:
    struct Sent {
        std::string          tag;
        std::vector<uint8_t> payload;
    };

    explicit FakeTransport(TransportHandler handler = TransportHandler())
        : handler_(std::move(handler)) {}

    // ---- Transport ----
    bool start() override {
        running_ = start_result_;
        if (running_) {
            for (const auto& tag : link_tags_) {
                if (handler_.on_link_up) {
                    handler_.on_link_up(tag);
                }
            }
        }
        return running_;
    }

    void stop() override {
        running_ = false;
    }

    bool is_running() const override {
        return running_;
    }

    bool send(const std::string& tag, const std::vector<uint8_t>& payload) override {
        if (!running_ || !has_link(tag)) {
            return false;
        }
        sent_.push_back(Sent{tag, payload});
        if (handler_.on_sent) {
            handler_.on_sent(tag, payload);
        }
        return true;
    }

    bool broadcast(const std::vector<uint8_t>& payload) override {
        if (!running_ || link_tags_.empty()) {
            return false;
        }
        for (const auto& tag : link_tags_) {
            send(tag, payload);
        }
        return true;
    }

    bool send_to(const std::string& host, uint16_t port,
                 const std::vector<uint8_t>& payload) override {
        if (!running_) {
            return false;
        }
        const std::string tag = host + ":" + std::to_string(port);
        sent_.push_back(Sent{tag, payload});
        if (handler_.on_sent) {
            handler_.on_sent(tag, payload);
        }
        return true;
    }

    std::vector<std::string> links() const override {
        return running_ ? link_tags_ : std::vector<std::string>();
    }

    // ---- test controls ----
    void set_start_result(bool ok) { start_result_ = ok; }

    void add_link(const std::string& tag) {
        if (!has_link(tag)) {
            link_tags_.push_back(tag);
        }
    }

    void remove_link(const std::string& tag) {
        for (auto it = link_tags_.begin(); it != link_tags_.end(); ++it) {
            if (*it == tag) {
                link_tags_.erase(it);
                return;
            }
        }
    }

    void inject_frame(const std::string& tag, const uint8_t* data, std::size_t len) {
        if (handler_.on_frame) {
            handler_.on_frame(tag, data, len);
        }
    }

    void inject_frame(const std::string& tag, const std::string& payload) {
        inject_frame(tag, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    }

    void inject_link_down(const std::string& tag, const std::string& why = "closed") {
        remove_link(tag);
        if (handler_.on_link_down) {
            handler_.on_link_down(tag, why);
        }
    }

    void inject_error(const std::string& tag, int code, const std::string& detail) {
        if (handler_.on_error) {
            handler_.on_error(tag, code, detail);
        }
    }

    const std::vector<Sent>& sent() const { return sent_; }

    void clear_sent() { sent_.clear(); }

private:
    bool has_link(const std::string& tag) const {
        for (const auto& existing : link_tags_) {
            if (existing == tag) {
                return true;
            }
        }
        return false;
    }

    TransportHandler     handler_;
    std::vector<std::string> link_tags_;
    std::vector<Sent>    sent_;
    bool                 running_      = false;
    bool                 start_result_ = true;
};

}  // namespace sp

#endif  // !_SP_FAKE_TRANSPORT_H_
