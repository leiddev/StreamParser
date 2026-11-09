/*
 * Leid 2026.7
 *
 * Smoke test for the pluggable logging facility (StreamParserLog.h).
 * It does not touch any network / serial device, only the sink itself:
 *
 *   LogSmokeTest        -> prints "[OK] ..." lines, exits with code 0 on success
 *
 * The test also shows how an application wraps the callback to hand records to
 * its own logging backend.
 */
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <sp/StreamParserLog.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "[OK]   " << what << std::endl;
    } else {
        std::cout << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

std::vector<splog::Record> g_records;

void capture(const splog::Record& record) {
    g_records.push_back(record);
}

}  // namespace

int main() {
    // 1) Records reach the installed callback with level / component / message.
    g_records.clear();
    splog::set_level(splog::Level::trace);
    splog::set_callback(capture);

    SP_LOG_INFO("serial", "opened COM3 @ " << 115200 << " baud");
    SP_LOG_ERROR("udp", "bind failed: " << "address in use");

    check(g_records.size() == 2, "callback receives one record per event");
    if (g_records.size() == 2) {
        check(g_records[0].level == splog::Level::info, "first record is INFO");
        check(std::string(g_records[0].component) == "serial", "component is 'serial'");
        check(g_records[0].message == "opened COM3 @ 115200 baud", "message is formatted");
        check(g_records[1].level == splog::Level::error, "second record is ERROR");
        check(std::string(g_records[1].component) == "udp", "component is 'udp'");
        check(g_records[1].file != nullptr && g_records[1].line > 0, "file:line captured");
    }

    // 2) format() gives a stable single line representation.
    if (!g_records.empty()) {
        check(splog::format(g_records[1]) == "[ERROR] [udp] bind failed: address in use",
              "format() renders '[LEVEL] [component] message'");
    }

    // 3) Minimum level filters records before they reach the callback.
    g_records.clear();
    splog::set_level(splog::Level::warn);
    SP_LOG_DEBUG("tcp", "debug dropped");
    SP_LOG_INFO("tcp", "info dropped");
    SP_LOG_WARN("tcp", "warn kept");
    check(g_records.size() == 1 && g_records[0].level == splog::Level::warn,
          "level filter drops everything below WARN");

    // 4) Level::off silences the library completely.
    g_records.clear();
    splog::set_level(splog::Level::off);
    SP_LOG_ERROR("tcp", "must not appear");
    check(g_records.empty(), "Level::off silences all records");

    // 5) A throwing callback never escapes into the library.
    splog::set_level(splog::Level::info);
    splog::set_callback([](const splog::Record&) { throw std::runtime_error("boom"); });
    bool escaped = false;
    try {
        SP_LOG_ERROR("tcp-client", "connect failed: " << "refused");
    } catch (...) {
        escaped = true;
    }
    check(!escaped, "exception thrown by the callback is swallowed");

    // 6) Clearing the callback restores the legacy stderr sink.
    splog::clear_callback();
    std::cout << "[INFO] the next line is written by the built-in stderr fallback:"
              << std::endl;
    SP_LOG_ERROR("tcp-server", "fallback sink active (expected on stderr)");
    check(splog::callback() == nullptr, "clear_callback() uninstalls the sink");

    std::cout << (g_failures == 0 ? "ALL LOG TESTS PASSED" : "LOG TESTS FAILED")
              << std::endl;
    return g_failures == 0 ? 0 : 1;
}
