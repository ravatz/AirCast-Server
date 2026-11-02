#include "miracast/miracast_server.h"
#include "log.h"
#include "video/video_renderer.h"

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
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET(s) close(s)
#define INVALID_SOCK (-1)
#endif

namespace ap::miracast {

struct MiracastServer::Impl {
    socket_t tcp_sock = INVALID_SOCK;
    socket_t udp_sock = INVALID_SOCK;

    ~Impl() {
        if (tcp_sock != INVALID_SOCK) {
            CLOSE_SOCKET(tcp_sock);
            tcp_sock = INVALID_SOCK;
        }
        if (udp_sock != INVALID_SOCK) {
            CLOSE_SOCKET(udp_sock);
            udp_sock = INVALID_SOCK;
        }
    }
};

MiracastServer::MiracastServer() : impl_(std::make_unique<Impl>()) {}

MiracastServer::~MiracastServer() {
    stop();
}

bool MiracastServer::start(uint16_t rtsp_port, uint16_t rtp_port, ap::video::VideoRenderer* renderer) {
    if (running_.load()) return true;

    renderer_ = renderer;
    rtp_port_ = rtp_port;
    running_.store(true);

    rtsp_thread_ = std::thread(&MiracastServer::run_rtsp, this, rtsp_port);
    rtp_thread_  = std::thread(&MiracastServer::run_rtp, this, rtp_port);

    LOG_INFO << "MiracastServer: listening on RTSP TCP " << rtsp_port << " and RTP UDP " << rtp_port << " for Android / Smart View";
    return true;
}

void MiracastServer::stop() {
    if (!running_.exchange(false)) return;

    if (impl_->tcp_sock != INVALID_SOCK) {
        CLOSE_SOCKET(impl_->tcp_sock);
        impl_->tcp_sock = INVALID_SOCK;
    }
    if (impl_->udp_sock != INVALID_SOCK) {
        CLOSE_SOCKET(impl_->udp_sock);
        impl_->udp_sock = INVALID_SOCK;
    }

    if (rtsp_thread_.joinable()) rtsp_thread_.join();
    if (rtp_thread_.joinable())  rtp_thread_.join();
}

void MiracastServer::run_rtsp(uint16_t port) {
    impl_->tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->tcp_sock == INVALID_SOCK) {
        LOG_ERROR << "MiracastServer: TCP socket() failed";
        return;
    }

    int opt = 1;
    setsockopt(impl_->tcp_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in sin{};
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(impl_->tcp_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        LOG_ERROR << "MiracastServer: bind() on port " << port << " failed";
        CLOSE_SOCKET(impl_->tcp_sock);
        impl_->tcp_sock = INVALID_SOCK;
        return;
    }

    if (listen(impl_->tcp_sock, 5) < 0) {
        LOG_ERROR << "MiracastServer: listen() failed";
        CLOSE_SOCKET(impl_->tcp_sock);
        impl_->tcp_sock = INVALID_SOCK;
        return;
    }

    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl_->tcp_sock, &rfds);

        timeval tv{1, 0};
        int sel = select(static_cast<int>(impl_->tcp_sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        socket_t client_sock = accept(impl_->tcp_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == INVALID_SOCK) continue;

        LOG_INFO << "MiracastServer: accepted RTSP client for Android Cast";

        char buf[4096];
        while (running_.load()) {
            int n = recv(client_sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';

            std::string req(buf);
            int cseq = 1;
            auto cseq_pos = req.find("CSeq: ");
            if (cseq_pos != std::string::npos) {
                cseq = std::atoi(req.c_str() + cseq_pos + 6);
            }

            std::ostringstream resp;
            if (req.rfind("OPTIONS", 0) == 0) {
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n"
                     << "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER, SETUP, PLAY, TEARDOWN\r\n\r\n";
            } else if (req.rfind("GET_PARAMETER", 0) == 0) {
                std::string body =
                    "wfd_video_formats: 00 00 02 02 00000080 00000000 00000000 00 0000 0000 00 none none\r\n"
                    "wfd_audio_codecs: LPCM 00000002 00, AAC 00000001 00\r\n"
                    "wfd_client_rtp_ports: RTP/AVP/UDP;unicast " + std::to_string(rtp_port_) + " 0 mode=play\r\n"
                    "wfd_content_protection: none\r\n"
                    "wfd_uibc_capability: none\r\n";

                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n"
                     << "Content-Type: text/parameters\r\n"
                     << "Content-Length: " << body.size() << "\r\n\r\n"
                     << body;
            } else if (req.rfind("SET_PARAMETER", 0) == 0) {
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n\r\n";
            } else if (req.rfind("SETUP", 0) == 0) {
                std::string client_port = "19000";
                auto cp_pos = req.find("client_port=");
                if (cp_pos != std::string::npos) {
                    auto end_pos = req.find_first_of(";-\r\n", cp_pos + 12);
                    client_port = req.substr(cp_pos + 12, end_pos - (cp_pos + 12));
                }
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n"
                     << "Session: 87654321;timeout=60\r\n"
                     << "Transport: RTP/AVP/UDP;unicast;client_port=" << client_port
                     << ";server_port=" << rtp_port_ << "\r\n\r\n";
            } else if (req.rfind("PLAY", 0) == 0) {
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n"
                     << "Session: 87654321\r\n\r\n";
                LOG_INFO << "MiracastServer: PLAY initiated — Android Cast streaming started";
            } else if (req.rfind("TEARDOWN", 0) == 0) {
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n"
                     << "Session: 87654321\r\n\r\n";
            } else {
                resp << "RTSP/1.0 200 OK\r\n"
                     << "CSeq: " << cseq << "\r\n\r\n";
            }

            std::string out = resp.str();
            send(client_sock, out.data(), static_cast<int>(out.size()), 0);

            if (req.rfind("TEARDOWN", 0) == 0) break;
        }

        CLOSE_SOCKET(client_sock);
        LOG_INFO << "MiracastServer: RTSP client disconnected";
    }
}

void MiracastServer::run_rtp(uint16_t port) {
    impl_->udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->udp_sock == INVALID_SOCK) {
        LOG_ERROR << "MiracastServer: UDP socket() failed";
        return;
    }

    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(impl_->udp_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    sockaddr_in sin{};
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(impl_->udp_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        LOG_ERROR << "MiracastServer: UDP bind() on port " << port << " failed";
        CLOSE_SOCKET(impl_->udp_sock);
        impl_->udp_sock = INVALID_SOCK;
        return;
    }

    uint8_t packet[2048];
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl_->udp_sock, &rfds);

        timeval tv{1, 0};
        int sel = select(static_cast<int>(impl_->udp_sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in from_addr{};
        socklen_t from_len = sizeof(from_addr);
        int n = recvfrom(impl_->udp_sock, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                         reinterpret_cast<sockaddr*>(&from_addr), &from_len);
        if (n <= 12) continue; // Minimum RTP header size

        // RTP header: packet[0..11]. Payload begins at packet[12] containing MPEG-TS packets (188 bytes)
        // Transport Stream sync byte is 0x47
        const uint8_t* payload = packet + 12;
        int payload_len = n - 12;

        if (payload_len >= 188 && payload[0] == 0x47) {
            // Valid MPEG-TS payload received
        }
    }
}

} // namespace ap::miracast
