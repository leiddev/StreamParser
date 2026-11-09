/*
 * TransportSerial.cc - Serial transport for the Transport facade.
 *
 * Leid 2026.10
 *
 * The device is the only link. A missing device does not fail the transport:
 * the port is reopened every reconnect_delay, matching what a hot-pluggable
 * collector needs.
 */
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <sp/SerialParserAdapter.h>
#include <sp/SerialTypes.h>
#include <sp/StreamParserLog.h>
#include "TransportImpl.h"

namespace sp {
namespace detail {

namespace {

SerialConfig toSerialConfig(const TransportConfig& cfg) {
    SerialConfig sc;
    sc.baud_rate = cfg.baud_rate;
    switch (cfg.parity) {
        case LineParity::Odd:  sc.parity = SerialConfig::Parity::Odd;  break;
        case LineParity::Even: sc.parity = SerialConfig::Parity::Even; break;
        default:               sc.parity = SerialConfig::Parity::None; break;
    }
    switch (cfg.stop_bits) {
        case LineStopBits::OnePointFive: sc.stop_bits = SerialConfig::StopBits::OnePointFive; break;
        case LineStopBits::Two:          sc.stop_bits = SerialConfig::StopBits::Two;          break;
        default:                         sc.stop_bits = SerialConfig::StopBits::One;          break;
    }
    switch (cfg.flow_control) {
        case LineFlowControl::Software: sc.flow_control = SerialConfig::FlowControl::Software; break;
        case LineFlowControl::Hardware: sc.flow_control = SerialConfig::FlowControl::Hardware; break;
        default:                        sc.flow_control = SerialConfig::FlowControl::None;     break;
    }
    return sc;
}

}  // namespace

class SerialTransport : public TransportBase {
public:
    SerialTransport(TransportConfig cfg, ParserFactory factory, TransportHandler handler)
        : TransportBase(std::move(cfg), std::move(factory), std::move(handler))
        , retry_(io()) {}

protected:
    bool open() override {
        return openDevice();
    }

    void close() override {
        retry_.cancel();
        if (serial_) {
            serial_->close();
            serial_.reset();
        }
    }

private:
    bool openDevice() {
        if (stopping()) {
            return true;
        }
        const std::string device = config().device;
        if (device.empty()) {
            emitError(config().name, -3, "serial device is not configured");
            return false;
        }

        auto parser = makeParser(device);
        try {
            serial_ = std::make_shared<SerialParserAdapter>(io(), device, toSerialConfig(config()),
                                                            std::move(parser));
        } catch (const std::exception& e) {
            /* A device that is currently unplugged is not a fatal error. */
            emitError(device, -4, std::string("cannot open ") + device + ": " + e.what());
            scheduleRetry();
            return true;
        }

        addLink(device, [this](const Frame& frame, WriteDone done) {
            std::shared_ptr<SerialParserAdapter> serial = serial_;
            if (!serial) {
                done(false, "serial port is closed");
                return;
            }
            serial->async_write(frame, [this, done](SerialWriteResult result, size_t /*bytes*/,
                                                    const asio::error_code& ec,
                                                    const std::string&      detail) {
                const bool ok = (result == SerialWriteResult::Ok);
                if (!ok) {
                    done(false, detail.empty() ? ec.message() : detail);
                    if (result == SerialWriteResult::PortClosed ||
                        result == SerialWriteResult::IOError) {
                        /* Device went away: release the port and wait for it. */
                        if (serial_) {
                            serial_->close();
                            serial_.reset();
                        }
                        scheduleRetry();
                    }
                    return;
                }
                done(true, std::string());
            });
        });

        if (config().log_link_events) {
            SP_LOG_INFO("transport",
                        device << ": serial port opened, " << config().baud_rate << " baud");
        }
        emitLinkUp(device);
        return true;
    }

    void scheduleRetry() {
        if (stopping()) {
            return;
        }
        const uint32_t delay = config().reopen_delay_ms;
        SP_LOG_WARN("transport", config().device << ": retrying in " << delay << " ms");
        retry_.expires_after(std::chrono::milliseconds(delay));
        retry_.async_wait([this](const asio::error_code& ec) {
            if (!ec && !stopping()) {
                openDevice();
            }
        });
    }

    std::shared_ptr<SerialParserAdapter> serial_;
    asio::steady_timer                   retry_;
};

}  // namespace detail

std::unique_ptr<Transport> MakeSerialTransport(TransportConfig  cfg,
                                               ParserFactory    factory,
                                               TransportHandler handler) {
    return std::unique_ptr<Transport>(
        new detail::SerialTransport(std::move(cfg), std::move(factory), std::move(handler)));
}

}  // namespace sp
