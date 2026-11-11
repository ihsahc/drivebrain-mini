#include <FlowmeterComms.hpp>
#include <FoxgloveServer.hpp>
#include <MCAPLogger.hpp>
#include <drivebrain_msgs.pb.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string_view>
#include <memory>
#include <stdexcept>
#include <thread>

volatile std::sig_atomic_t running = 1;

void sig_handler(int) {
    running = 0;
}

int main() {
    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);
    int status = 0;
    try {
        const char* fake_pulses = std::getenv("USE_FAKE_PULSES");
        FlowmeterComms flowmeter(fake_pulses && std::string_view(fake_pulses) == "1");
        core::MCAPLogger::create(mcap::McapWriterOptions(""));
        if (core::MCAPLogger::instance().open_new_mcap() != 0) {
            throw std::runtime_error("Could not open MCAP recording");
        }
        core::MCAPLogger::instance().init_logging();
        core::FoxgloveServer::create();

        auto log_pulses = [&] {
            auto msg = std::make_shared<drivebrain_msgs::FlowmeterData>();
            msg->set_pulse_count(flowmeter.latest_pulse_count());
            core::MCAPLogger::instance().log_msg(msg);
            core::FoxgloveServer::instance().send_live_telem_msg(msg);
        };
        while (running && !flowmeter.failed()) {
            log_pulses();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        flowmeter.stop(); // Join the reader, then drain the pulse queue.
        log_pulses(); // Save the final count before closing the logger.
        if (flowmeter.failed()) status = 1;
    } catch (const std::exception& error) {
        spdlog::error("{}", error.what());
        status = 1;
    }
    core::MCAPLogger::destroy();
    core::FoxgloveServer::destroy();
    return status;
}
