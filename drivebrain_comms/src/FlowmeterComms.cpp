#include <FlowmeterComms.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

// BCM GPIO 12 is physical header pin 32. Check the chip on your Pi with gpioinfo.
static constexpr const char* GPIO_CHIP = "/dev/gpiochip0";
static constexpr unsigned int GPIO_LINE = 12;

FlowmeterComms::FlowmeterComms(bool use_fake_pulses) {
    if (!use_fake_pulses) {
        int chip_fd = open(GPIO_CHIP, O_RDONLY | O_CLOEXEC);
        if (chip_fd < 0) {
            throw std::runtime_error(std::string("Open GPIO chip: ") + std::strerror(errno));
        }
        gpio_v2_line_request request{};
        request.offsets[0] = GPIO_LINE;
        request.num_lines = 1;
        std::strcpy(request.consumer, "isha");
        request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                               GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
        int result = ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request);
        int error = errno;
        close(chip_fd);
        if (result < 0) {
            throw std::runtime_error(std::string("Request GPIO line: ") + std::strerror(error));
        }
        _gpio_fd = request.fd;
    }
    try {
        _handler_thread = std::thread(&FlowmeterComms::_handle_loop, this);
        _reader_thread = std::thread(use_fake_pulses ? &FlowmeterComms::_read_fake_loop
                                                   : &FlowmeterComms::_read_gpio_loop, this);
    } catch (...) {
        stop();
        throw;
    }
}

FlowmeterComms::~FlowmeterComms() {
    stop();
}

void FlowmeterComms::stop() {
    _stop = true;
    if (_reader_thread.joinable()) _reader_thread.join();
    {
        std::lock_guard<std::mutex> lock(_queue.mtx);
        _reader_done = true;
    }
    _queue.cv.notify_one();
    if (_handler_thread.joinable()) _handler_thread.join();
    if (_gpio_fd >= 0) {
        close(_gpio_fd);
        _gpio_fd = -1;
    }
}

void FlowmeterComms::_push_pulse(uint64_t timestamp_ns) {
    {
        std::lock_guard<std::mutex> lock(_queue.mtx);
        _queue.deque.push_back(timestamp_ns);
    }
    _queue.cv.notify_one();
}

void FlowmeterComms::_read_fake_loop() {
    while (!_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (_stop) break;
        _push_pulse(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

void FlowmeterComms::_read_gpio_loop() {
    pollfd pin{_gpio_fd, POLLIN, 0};
    while (!_stop) {
        int result = poll(&pin, 1, 100);
        if (result < 0 && errno == EINTR) continue;
        if (result == 0) continue;
        if (result < 0 || (pin.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            spdlog::error("GPIO polling failed");
            _failed = true;
            break;
        }
        gpio_v2_line_event event{};
        auto bytes = read(_gpio_fd, &event, sizeof(event));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes != sizeof(event)) {
            spdlog::error("GPIO event read failed");
            _failed = true;
            break;
        }
        _push_pulse(event.timestamp_ns);
    }
}

void FlowmeterComms::_handle_loop() {
    std::deque<uint64_t> pulses;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(_queue.mtx);
            _queue.cv.wait(lock, [this] { return !_queue.deque.empty() || _reader_done; });
            if (_reader_done && _queue.deque.empty()) return;
            pulses.swap(_queue.deque);
        }
        _count += pulses.size();
        pulses.clear();
    }
}
