#pragma once
#include <DriverBus.hpp>
#include <atomic>
#include <cstdint>
#include <thread>

class FlowmeterComms {
public:
    explicit FlowmeterComms(bool use_fake_pulses);
    ~FlowmeterComms();
    void stop();
    uint64_t latest_pulse_count() const { return _count.load(); }
    bool failed() const { return _failed.load(); }

private:
    void _read_fake_loop();
    void _push_pulse(uint64_t timestamp_ns);
    void _read_gpio_loop();
    void _handle_loop();

    core::ThreadSafeDeque<uint64_t> _queue;
    std::atomic<uint64_t> _count{0};
    std::atomic<bool> _stop{false};
    std::atomic<bool> _failed{false};
    bool _reader_done = false; // Protected by the queue mutex.
    int _gpio_fd = -1;
    std::thread _reader_thread;
    std::thread _handler_thread;
};
