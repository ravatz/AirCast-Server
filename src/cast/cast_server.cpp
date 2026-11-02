#include "cast/cast_server.h"
#include "cast/cast_channel.h"
#include "log.h"
#include "video/video_renderer.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/rsa.h>

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

#include <vector>

namespace ap::cast {

struct CastServer::Impl {
    SSL_CTX* ssl_ctx = nullptr;
    socket_t server_sock = INVALID_SOCK;

    ~Impl() {
        if (server_sock != INVALID_SOCK) {
            CLOSE_SOCKET(server_sock);
            server_sock = INVALID_SOCK;
        }
        if (ssl_ctx) {
            SSL_CTX_free(ssl_ctx);
            ssl_ctx = nullptr;
        }
    }

    bool init_ssl() {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();

        const SSL_METHOD* method = TLS_server_method();
        ssl_ctx = SSL_CTX_new(method);
        if (!ssl_ctx) {
            LOG_ERROR << "CastServer: SSL_CTX_new failed";
            return false;
        }

        EVP_PKEY* pkey = nullptr;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        pkey = EVP_RSA_gen(2048);
#endif
        if (!pkey) {
            pkey = EVP_PKEY_new();
            BIGNUM* e = BN_new();
            BN_set_word(e, RSA_F4);
            RSA* rsa = RSA_new();
            RSA_generate_key_ex(rsa, 2048, e, nullptr);
            EVP_PKEY_assign_RSA(pkey, rsa);
            BN_free(e);
        }

        X509* x509 = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), 315360000L); // ~10 years
        X509_set_pubkey(x509, pkey);

        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("AirCast Server"), -1, -1, 0);
        X509_set_issuer_name(x509, name);
        X509_sign(x509, pkey, EVP_sha256());

        if (SSL_CTX_use_certificate(ssl_ctx, x509) <= 0 ||
            SSL_CTX_use_PrivateKey(ssl_ctx, pkey) <= 0) {
            LOG_ERROR << "CastServer: failed to attach self-signed cert";
            X509_free(x509);
            EVP_PKEY_free(pkey);
            return false;
        }

        X509_free(x509);
        EVP_PKEY_free(pkey);
        return true;
    }
};

CastServer::CastServer() : impl_(std::make_unique<Impl>()) {}

CastServer::~CastServer() {
    stop();
}

bool CastServer::start(uint16_t port, ap::video::VideoRenderer* renderer) {
    if (running_.load()) return true;

    renderer_ = renderer;
    if (!impl_->init_ssl()) {
        LOG_ERROR << "CastServer: SSL init failed";
        return false;
    }

    running_.store(true);
    server_thread_ = std::thread(&CastServer::run, this, port);
    LOG_INFO << "CastServer: listening on port " << port << " for ChromeOS / Google Cast";
    return true;
}

void CastServer::stop() {
    if (!running_.exchange(false)) return;

    if (impl_->server_sock != INVALID_SOCK) {
        CLOSE_SOCKET(impl_->server_sock);
        impl_->server_sock = INVALID_SOCK;
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void CastServer::run(uint16_t port) {
    impl_->server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->server_sock == INVALID_SOCK) {
        LOG_ERROR << "CastServer: socket() failed";
        running_.store(false);
        return;
    }

    int opt = 1;
    setsockopt(impl_->server_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in sin{};
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (bind(impl_->server_sock, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        LOG_ERROR << "CastServer: bind() on port " << port << " failed";
        CLOSE_SOCKET(impl_->server_sock);
        impl_->server_sock = INVALID_SOCK;
        running_.store(false);
        return;
    }

    if (listen(impl_->server_sock, 5) < 0) {
        LOG_ERROR << "CastServer: listen() failed";
        CLOSE_SOCKET(impl_->server_sock);
        impl_->server_sock = INVALID_SOCK;
        running_.store(false);
        return;
    }

    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(impl_->server_sock, &rfds);

        timeval tv{1, 0};
        int sel = select(static_cast<int>(impl_->server_sock + 1), &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        socket_t client_sock = accept(impl_->server_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == INVALID_SOCK) continue;

        std::thread([this, client_sock]() {
            handle_client(static_cast<uint64_t>(client_sock));
        }).detach();
    }
}

static int extract_request_id(const std::string& json) {
    auto pos = json.find("\"requestId\":");
    if (pos == std::string::npos) pos = json.find("\"requestId\" :");
    if (pos != std::string::npos) {
        const char* p = json.c_str() + pos + 12;
        while (*p == ' ' || *p == ':') ++p;
        return std::atoi(p);
    }
    return 1;
}

void CastServer::handle_client(uint64_t raw_sock) {
    socket_t client_sock = static_cast<socket_t>(raw_sock);
    LOG_INFO << "CastServer: accepted client connection";

    SSL* ssl = SSL_new(impl_->ssl_ctx);
    SSL_set_fd(ssl, static_cast<int>(client_sock));

    if (SSL_accept(ssl) <= 0) {
        LOG_WARN << "CastServer: SSL_accept failed";
        SSL_free(ssl);
        CLOSE_SOCKET(client_sock);
        return;
    }

    // Handle CastV2 messages
    while (running_.load()) {
        uint8_t len_buf[4];
        int r = SSL_read(ssl, len_buf, 4);
        if (r <= 0) break;

        uint32_t msg_len = (static_cast<uint32_t>(len_buf[0]) << 24) |
                           (static_cast<uint32_t>(len_buf[1]) << 16) |
                           (static_cast<uint32_t>(len_buf[2]) << 8)  |
                           static_cast<uint32_t>(len_buf[3]);

        if (msg_len > 1024 * 1024) break; // sanity limit 1MB

        std::vector<uint8_t> body(msg_len);
        size_t read_bytes = 0;
        while (read_bytes < msg_len) {
            int n = SSL_read(ssl, body.data() + read_bytes, static_cast<int>(msg_len - read_bytes));
            if (n <= 0) break;
            read_bytes += n;
        }
        if (read_bytes < msg_len) break;

        CastMessage in_msg;
        if (!decode_cast_message(body.data(), body.size(), in_msg)) continue;

        // Heartbeat namespace
        if (in_msg.ns == kHeartbeatNamespace) {
            if (in_msg.payload_utf8.find("\"PING\"") != std::string::npos) {
                CastMessage pong;
                pong.source_id      = in_msg.destination_id;
                pong.destination_id = in_msg.source_id;
                pong.ns             = kHeartbeatNamespace;
                pong.payload_utf8   = "{\"type\":\"PONG\"}";
                auto framed = encode_framed_message(pong);
                SSL_write(ssl, framed.data(), static_cast<int>(framed.size()));
            }
        }
        // Receiver namespace (status & launch)
        else if (in_msg.ns == kReceiverNamespace) {
            int req_id = extract_request_id(in_msg.payload_utf8);
            if (in_msg.payload_utf8.find("\"GET_STATUS\"") != std::string::npos) {
                CastMessage status_msg;
                status_msg.source_id      = in_msg.destination_id;
                status_msg.destination_id = in_msg.source_id;
                status_msg.ns             = kReceiverNamespace;
                status_msg.payload_utf8   = "{\"type\":\"RECEIVER_STATUS\",\"requestId\":" + std::to_string(req_id) + ","
                                            "\"status\":{\"volume\":{\"controlType\":\"attenuation\",\"level\":1.0,\"muted\":false,\"stepInterval\":0.05},"
                                            "\"userEq\":{}}}";
                auto framed = encode_framed_message(status_msg);
                SSL_write(ssl, framed.data(), static_cast<int>(framed.size()));
            } else if (in_msg.payload_utf8.find("\"LAUNCH\"") != std::string::npos) {
                CastMessage launch_resp;
                launch_resp.source_id      = in_msg.destination_id;
                launch_resp.destination_id = in_msg.source_id;
                launch_resp.ns             = kReceiverNamespace;
                launch_resp.payload_utf8   = "{\"type\":\"RECEIVER_STATUS\",\"requestId\":" + std::to_string(req_id) + ","
                                             "\"status\":{\"applications\":[{"
                                             "\"appId\":\"AirCast\",\"displayName\":\"AirCast Server\","
                                             "\"isIdleScreen\":false,\"sessionId\":\"aircast-session\","
                                             "\"statusText\":\"Casting\",\"transportId\":\"web-1\","
                                             "\"namespaces\":[{\"name\":\"urn:x-cast:com.google.cast.media\"}]"
                                             "}],\"volume\":{\"controlType\":\"attenuation\",\"level\":1.0,\"muted\":false,\"stepInterval\":0.05}}}";
                auto framed = encode_framed_message(launch_resp);
                SSL_write(ssl, framed.data(), static_cast<int>(framed.size()));
            }
        }
        // Media namespace
        else if (in_msg.ns == kMediaNamespace) {
            int req_id = extract_request_id(in_msg.payload_utf8);
            CastMessage media_resp;
            media_resp.source_id      = in_msg.destination_id;
            media_resp.destination_id = in_msg.source_id;
            media_resp.ns             = kMediaNamespace;
            media_resp.payload_utf8   = "{\"type\":\"MEDIA_STATUS\",\"status\":[],\"requestId\":" + std::to_string(req_id) + "}";
            auto framed = encode_framed_message(media_resp);
            SSL_write(ssl, framed.data(), static_cast<int>(framed.size()));
        }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    CLOSE_SOCKET(client_sock);
    LOG_INFO << "CastServer: client connection closed";
}

} // namespace ap::cast
