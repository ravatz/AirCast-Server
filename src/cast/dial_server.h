#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace ap::cast {

class DialServer {
public:
    DialServer();
    ~DialServer();

    DialServer(const DialServer&) = delete;
    DialServer& operator=(const DialServer&) = delete;

    bool start(const std::string& name, const std::string& uuid, const std::string& local_ip, uint16_t http_port = 8008);
    void stop();

    bool is_running() const { return running_.load(); }

private:
    void run_ssdp(uint16_t http_port);
    void run_http(uint16_t http_port);
    void handle_http_client(uint64_t client_sock);

    std::string name_;
    std::string uuid_;
    std::string local_ip_;
    uint16_t http_port_{8008};

    std::atomic<bool> running_{false};
    std::thread ssdp_thread_;
    std::thread http_thread_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::cast
