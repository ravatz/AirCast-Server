#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace ap::video {
class VideoRenderer;
}

namespace ap::miracast {

class MiracastServer {
public:
    MiracastServer();
    ~MiracastServer();

    MiracastServer(const MiracastServer&) = delete;
    MiracastServer& operator=(const MiracastServer&) = delete;

    bool start(uint16_t rtsp_port, uint16_t rtp_port, ap::video::VideoRenderer* renderer);
    void stop();

    bool is_running() const { return running_.load(); }

private:
    void run_rtsp(uint16_t rtsp_port);
    void run_rtp(uint16_t rtp_port);

    std::atomic<bool> running_{false};
    std::thread rtsp_thread_;
    std::thread rtp_thread_;
    uint16_t rtp_port_{19000};
    ap::video::VideoRenderer* renderer_{nullptr};

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::miracast
