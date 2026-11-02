#include "cast/dial_server.h"
#include "log.h"

#include <cstring>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define CLOSE_SOCKET(s) closesocket(s)
#define INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCK (-1)
#endif

namespace ap::cast {

struct DialServer::Impl {
    socket_t ssdp_sock = INVALID_SOCK;
    socket_t http_sock = INVALID_SOCK;

    ~Impl() {
        if (ssdp_sock != INVALID_SOCK) {
            CLOSE_SOCKET(ssdp_sock);
            ssdp_sock = INVALID_SOCK;
        }
        if (http_sock != INVALID_SOCK) {
            CLOSE_SOCKET(http_sock);
            http_sock = INVALID_SOCK;
        }
    }
};

DialServer::DialServer() : impl_(std::make_unique<Impl>()) {}

DialServer::~DialServer() {
    stop();
}

bool DialServer::start(const std::string& name, const std::string& uuid, const std::string& local_ip, uint16_t http_port) {
    if (running_.load()) return true;

    name_ = name;
    uuid_ = uuid;
    local_ip_ = local_ip.empty() ? "0.0.0.0" : local_ip;
    http_port_ = http_port;
    running_.store(true);

    http_thread_ = std::thread(&DialServer::run_http, this, http_port);
    ssdp_thread_ = std::thread(&DialServer::run_ssdp, this, http_port);

    LOG_INFO << "DialServer: listening on HTTP port " << http_port << " and SSDP UDP 1900";
    return true;
}

void DialServer::stop() {
    if (!running_.exchange(false)) return;

    if (impl_->ssdp_sock != INVALID_SOCK) {
        CLOSE_SOCKET(impl_->ssdp_sock);
        impl_->ssdp_sock = INVALID_SOCK;
    }
    if (impl_->http_sock != INVALID_SOCK) {
        CLOSE_SOCKET(impl_->http_sock);
        impl_->http_sock = INVALID_SOCK;
    }

    if (ssdp_thread_.joinable()) ssdp_thread_.join();
    if (http_thread_.joinable()) http_thread_.join();
}

void DialServer::run_ssdp(uint16_t http_port) {
    impl_->ssdp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->ssdp_sock == INVALID_SOCK) {
        LOG_WARN << "DialServer: SSDP UDP socket creation failed";
        return;
    }

    int reuse = 1;
    setsockopt(impl_->ssdp_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(1900);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(impl_->ssdp_sock, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
        LOG_WARN << "DialServer: SSDP bind to UDP 1900 failed (may already be bound by Windows SSDP service)";
        CLOSE_SOCKET(impl_->ssdp_sock);
        impl_->ssdp_sock = INVALID_SOCK;
        return;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(impl_->ssdp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

    char buf[4096];
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl_->ssdp_sock, &rfds);

        timeval tv{1, 0};
        int sel = select(static_cast<int>(impl_->ssdp_sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in sender_addr{};
        socklen_t sender_len = sizeof(sender_addr);
        int n = recvfrom(impl_->ssdp_sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
        if (n <= 0) continue;
        buf[n] = '\0';

        std::string req(buf);
        if (req.find("M-SEARCH") != std::string::npos &&
            (req.find("urn:dial-multiscreen-org:service:dial:1") != std::string::npos ||
             req.find("ssdp:all") != std::string::npos)) {

            std::string ip_str = local_ip_;
            if (ip_str.empty() || ip_str == "0.0.0.0") {
                sockaddr_in local_sock_name{};
                socklen_t local_name_len = sizeof(local_sock_name);
                if (getsockname(impl_->ssdp_sock, reinterpret_cast<sockaddr*>(&local_sock_name), &local_name_len) == 0) {
                    char ip_buf[INET_ADDRSTRLEN];
                    if (inet_ntop(AF_INET, &local_sock_name.sin_addr, ip_buf, sizeof(ip_buf))) {
                        ip_str = ip_buf;
                    }
                }
            }
            if (ip_str.empty() || ip_str == "0.0.0.0") {
                ip_str = "127.0.0.1";
            }

            std::ostringstream resp;
            resp << "HTTP/1.1 200 OK\r\n"
                 << "CACHE-CONTROL: max-age=1800\r\n"
                 << "DATE: \r\n"
                 << "EXT: \r\n"
                 << "LOCATION: http://" << ip_str << ":" << http_port << "/ssdp/device-desc.xml\r\n"
                 << "SERVER: Linux/3.8.13, UPnP/1.0, Portable SDK for UPnP devices/1.6.18\r\n"
                 << "ST: urn:dial-multiscreen-org:service:dial:1\r\n"
                 << "USN: uuid:" << uuid_ << "::urn:dial-multiscreen-org:service:dial:1\r\n"
                 << "BOOTID.UPNP.ORG: 1\r\n"
                 << "CONFIGID.UPNP.ORG: 1\r\n\r\n";

            std::string resp_str = resp.str();
            sendto(impl_->ssdp_sock, resp_str.data(), static_cast<int>(resp_str.size()), 0,
                   reinterpret_cast<sockaddr*>(&sender_addr), sender_len);
        }
    }
}

void DialServer::run_http(uint16_t http_port) {
    impl_->http_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->http_sock == INVALID_SOCK) {
        LOG_WARN << "DialServer: HTTP socket creation failed";
        return;
    }

    int opt = 1;
    setsockopt(impl_->http_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(http_port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(impl_->http_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        LOG_WARN << "DialServer: HTTP bind to port " << http_port << " failed";
        CLOSE_SOCKET(impl_->http_sock);
        impl_->http_sock = INVALID_SOCK;
        return;
    }

    if (listen(impl_->http_sock, 10) < 0) {
        LOG_WARN << "DialServer: HTTP listen failed";
        CLOSE_SOCKET(impl_->http_sock);
        impl_->http_sock = INVALID_SOCK;
        return;
    }

    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl_->http_sock, &rfds);

        timeval tv{1, 0};
        int sel = select(static_cast<int>(impl_->http_sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        socket_t client_sock = accept(impl_->http_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == INVALID_SOCK) continue;

        std::thread([this, client_sock]() {
            handle_http_client(static_cast<uint64_t>(client_sock));
        }).detach();
    }
}

void DialServer::handle_http_client(uint64_t raw_sock) {
    socket_t client_sock = static_cast<socket_t>(raw_sock);

    char buf[4096];
    int n = recv(client_sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        CLOSE_SOCKET(client_sock);
        return;
    }
    buf[n] = '\0';
    std::string req(buf);

    std::string method, path;
    {
        std::istringstream iss(req);
        iss >> method >> path;
    }

    std::string response;

    if (path == "/ssdp/device-desc.xml" || path.rfind("/ssdp/device-desc.xml", 0) == 0) {
        std::string xml =
            "<?xml version=\"1.0\"?>\r\n"
            "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
            "  <specVersion>\r\n"
            "    <major>1</major>\r\n"
            "    <minor>0</minor>\r\n"
            "  </specVersion>\r\n"
            "  <device>\r\n"
            "    <deviceType>urn:dial-multiscreen-org:device:dial:1</deviceType>\r\n"
            "    <friendlyName>" + name_ + "</friendlyName>\r\n"
            "    <manufacturer>Google Inc.</manufacturer>\r\n"
            "    <modelName>Eureka Dongle</modelName>\r\n"
            "    <UDN>uuid:" + uuid_ + "</UDN>\r\n"
            "    <iconList>\r\n"
            "      <icon>\r\n"
            "        <mimetype>image/png</mimetype>\r\n"
            "        <width>98</width>\r\n"
            "        <height>55</height>\r\n"
            "        <depth>32</depth>\r\n"
            "        <url>/setup/icon.png</url>\r\n"
            "      </icon>\r\n"
            "    </iconList>\r\n"
            "    <serviceList>\r\n"
            "      <service>\r\n"
            "        <serviceType>urn:dial-multiscreen-org:service:dial:1</serviceType>\r\n"
            "        <serviceId>urn:dial-multiscreen-org:serviceId:dial</serviceId>\r\n"
            "        <controlURL>/ssdp/notfound</controlURL>\r\n"
            "        <eventSubURL>/ssdp/notfound</eventSubURL>\r\n"
            "        <SCPDURL>/ssdp/notfound</SCPDURL>\r\n"
            "      </service>\r\n"
            "    </serviceList>\r\n"
            "  </device>\r\n"
            "</root>\r\n";

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/xml\r\n"
            << "Application-URL: http://" << local_ip_ << ":" << http_port_ << "/apps/\r\n"
            << "Content-Length: " << xml.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << xml;
        response = oss.str();
    } else if (path == "/setup/eureka_info" || path.rfind("/setup/eureka_info", 0) == 0) {
        std::string json =
            "{\"name\":\"" + name_ + "\","
            "\"device_info\":{\"4k_blocked\":0,\"capabilities\":{\"video_out\":true,\"audio_out\":true},"
            "\"cloudcast_supported\":true,\"manufacturer\":\"Google Inc.\",\"model_name\":\"Eureka Dongle\","
            "\"ssdp_udn\":\"uuid:" + uuid_ + "\"},"
            "\"build_info\":{\"build_type\":2,\"cast_build_revision\":\"1.46.195690\",\"system_build_number\":\"195690\"},"
            "\"opt_in\":{\"opencast\":false,\"stats\":false}}";

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json; charset=utf-8\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Content-Length: " << json.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << json;
        response = oss.str();
    } else if (path == "/setup/icon.png") {
        std::ostringstream oss;
        oss << "HTTP/1.1 404 Not Found\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        response = oss.str();
    } else {
        std::ostringstream oss;
        oss << "HTTP/1.1 404 Not Found\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        response = oss.str();
    }

    send(client_sock, response.data(), static_cast<int>(response.size()), 0);
    CLOSE_SOCKET(client_sock);
}

} // namespace ap::cast
