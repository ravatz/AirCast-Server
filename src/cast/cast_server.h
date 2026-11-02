#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace ap::video {
class VideoRenderer;
}

namespace ap::cast {

class CastServer {
public:
    CastServer();
    ~CastServer();

    CastServer(const CastServer&) = delete;
    CastServer& operator=(const CastServer&) = delete;

    bool start(uint16_t port, ap::video::VideoRenderer* renderer);
    void stop();

    bool is_running() const { return running_.load(); }

private:
    void run(uint16_t port);
    void handle_client(uint64_t sock);

    std::atomic<bool> running_{false};
    std::thread server_thread_;
    ap::video::VideoRenderer* renderer_{nullptr};
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::cast
