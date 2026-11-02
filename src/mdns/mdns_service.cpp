#include "mdns/mdns_service.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <windns.h>
    #pragma comment(lib, "dnsapi.lib")
#endif

namespace ap::mdns {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

std::string narrow(PCWSTR ws) {
    if (!ws) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), len, nullptr, nullptr);
    return out;
}

// -----------------------------------------------------------------------------
// Dynamic Apple Bonjour (dnssd.dll) definitions
// -----------------------------------------------------------------------------
typedef struct _DNSServiceRef_t* DNSServiceRef;
typedef union _TXTRecordRef_t { char PrivateData[16]; char* ForceNaturalAlignment; } TXTRecordRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t  DNSServiceErrorType;

typedef void (__stdcall *DNSServiceRegisterReply)
    (
    DNSServiceRef           sdRef,
    DNSServiceFlags         flags,
    DNSServiceErrorType     errorCode,
    const char*             name,
    const char*             regtype,
    const char*             domain,
    void*                   context
    );

typedef DNSServiceErrorType (__stdcall *DNSServiceRegister_fn)
    (
    DNSServiceRef*          sdRef,
    DNSServiceFlags         flags,
    uint32_t                interfaceIndex,
    const char*             name,
    const char*             regtype,
    const char*             domain,
    const char*             host,
    uint16_t                port,
    uint16_t                txtLen,
    const void*             txtRecord,
    DNSServiceRegisterReply callBack,
    void*                   context
    );

typedef void (__stdcall *DNSServiceRefDeallocate_fn)(DNSServiceRef sdRef);
typedef void (__stdcall *TXTRecordCreate_fn)(TXTRecordRef* txtRecord, uint16_t bufferLen, void* buffer);
typedef void (__stdcall *TXTRecordDeallocate_fn)(TXTRecordRef* txtRecord);
typedef DNSServiceErrorType (__stdcall *TXTRecordSetValue_fn)(TXTRecordRef* txtRecord, const char* key, uint8_t valueSize, const void* value);
typedef uint16_t (__stdcall *TXTRecordGetLength_fn)(const TXTRecordRef* txtRecord);
typedef const void* (__stdcall *TXTRecordGetBytesPtr_fn)(const TXTRecordRef* txtRecord);

struct BonjourClient {
    HMODULE module = nullptr;
    DNSServiceRegister_fn      DNSServiceRegister = nullptr;
    DNSServiceRefDeallocate_fn DNSServiceRefDeallocate = nullptr;
    TXTRecordCreate_fn         TXTRecordCreate = nullptr;
    TXTRecordSetValue_fn       TXTRecordSetValue = nullptr;
    TXTRecordGetLength_fn      TXTRecordGetLength = nullptr;
    TXTRecordGetBytesPtr_fn    TXTRecordGetBytesPtr = nullptr;
    TXTRecordDeallocate_fn     TXTRecordDeallocate = nullptr;

    bool load() {
        module = LoadLibraryA("dnssd.dll");
        if (!module) return false;

        DNSServiceRegister = (DNSServiceRegister_fn)GetProcAddress(module, "DNSServiceRegister");
        DNSServiceRefDeallocate = (DNSServiceRefDeallocate_fn)GetProcAddress(module, "DNSServiceRefDeallocate");
        TXTRecordCreate = (TXTRecordCreate_fn)GetProcAddress(module, "TXTRecordCreate");
        TXTRecordSetValue = (TXTRecordSetValue_fn)GetProcAddress(module, "TXTRecordSetValue");
        TXTRecordGetLength = (TXTRecordGetLength_fn)GetProcAddress(module, "TXTRecordGetLength");
        TXTRecordGetBytesPtr = (TXTRecordGetBytesPtr_fn)GetProcAddress(module, "TXTRecordGetBytesPtr");
        TXTRecordDeallocate = (TXTRecordDeallocate_fn)GetProcAddress(module, "TXTRecordDeallocate");

        if (!DNSServiceRegister || !DNSServiceRefDeallocate || !TXTRecordCreate ||
            !TXTRecordSetValue || !TXTRecordGetLength || !TXTRecordGetBytesPtr || !TXTRecordDeallocate) {
            FreeLibrary(module);
            module = nullptr;
            return false;
        }
        return true;
    }

    ~BonjourClient() {
        if (module) FreeLibrary(module);
    }
};

} // namespace

// One registered service using Windows DnsServiceRegister
struct RegisteredService {
    std::wstring              instance_name;
    std::wstring              host_name;
    std::vector<std::wstring> keys;
    std::vector<std::wstring> values;
    std::vector<PWSTR>        key_ptrs;
    std::vector<PWSTR>        value_ptrs;
    DNS_SERVICE_INSTANCE      instance{};
    DNS_SERVICE_CANCEL        cancel{};
    DNS_SERVICE_REGISTER_REQUEST request{};
    std::mutex mutex;
    std::condition_variable cv;
    bool pending{false};
    bool deregistering{false};
    DWORD status{ERROR_SUCCESS};
    bool active{false};

    void fill_ptrs() {
        key_ptrs.clear();
        value_ptrs.clear();
        for (auto& s : keys)   key_ptrs.push_back(s.data());
        for (auto& s : values) value_ptrs.push_back(s.data());
    }
};

struct MdnsService::Impl {
    BonjourClient bonjour;
    bool use_bonjour = false;

    // Handles for Bonjour mode
    std::vector<DNSServiceRef> bonjour_refs;

    // Handles for native Windows fallback
    std::shared_ptr<RegisteredService> airplay = std::make_shared<RegisteredService>();
    std::shared_ptr<RegisteredService> raop = std::make_shared<RegisteredService>();
    std::shared_ptr<RegisteredService> googlecast = std::make_shared<RegisteredService>();
    std::shared_ptr<RegisteredService> miracast = std::make_shared<RegisteredService>();

    static void WINAPI on_complete(DWORD status, PVOID context,
                                   PDNS_SERVICE_INSTANCE inst) {
        std::unique_ptr<std::shared_ptr<RegisteredService>> owner(
            static_cast<std::shared_ptr<RegisteredService>*>(context));
        auto service = *owner;
        if (inst) DnsServiceFreeInstance(inst);
        std::lock_guard<std::mutex> lock(service->mutex);
        service->status = status;
        service->pending = false;
        LOG_INFO << "mDNS " << (service->deregistering ? "deregister" : "register")
                 << ": " << narrow(service->instance_name.c_str())
                 << " status=" << status;
        service->cv.notify_all();
    }

    bool register_service(const std::shared_ptr<RegisteredService>& service,
                          uint16_t port) {
        auto& s = *service;
        s.fill_ptrs();
        s.instance = {};
        s.instance.pszInstanceName = s.instance_name.data();
        s.instance.pszHostName = s.host_name.data();
        s.instance.wPort = port;
        s.instance.dwPropertyCount = static_cast<DWORD>(s.keys.size());
        s.instance.keys = s.key_ptrs.data();
        s.instance.values = s.value_ptrs.data();
        s.request = {};
        s.request.Version = DNS_QUERY_REQUEST_VERSION1;
        s.request.pServiceInstance = &s.instance;
        s.request.pRegisterCompletionCallback = &on_complete;
        s.request.pQueryContext = new std::shared_ptr<RegisteredService>(service);
        s.pending = true;
        DWORD rc = DnsServiceRegister(&s.request, &s.cancel);
        if (rc != DNS_REQUEST_PENDING) {
            delete static_cast<std::shared_ptr<RegisteredService>*>(s.request.pQueryContext);
            s.pending = false;
            LOG_ERROR << "DnsServiceRegister failed: " << rc;
            return false;
        }
        s.active = true;
        return true;
    }

    void unregister_service(const std::shared_ptr<RegisteredService>& service) {
        auto& s = *service;
        if (!s.active) return;
        s.active = false;
        std::unique_lock<std::mutex> lock(s.mutex);
        if (!s.cv.wait_for(lock, std::chrono::seconds(3), [&s] { return !s.pending; })) {
            lock.unlock();
            DnsServiceRegisterCancel(&s.cancel);
            lock.lock();
            if (!s.cv.wait_for(lock, std::chrono::seconds(3), [&s] { return !s.pending; })) {
                LOG_WARN << "mDNS registration cancellation still pending";
                return;
            }
        }
        if (s.status != ERROR_SUCCESS) return;
        s.deregistering = true;
        s.pending = true;
        s.request.pQueryContext = new std::shared_ptr<RegisteredService>(service);
        lock.unlock();
        DWORD rc = DnsServiceDeRegister(&s.request, nullptr);
        lock.lock();
        if (rc != DNS_REQUEST_PENDING) {
            delete static_cast<std::shared_ptr<RegisteredService>*>(s.request.pQueryContext);
            s.pending = false;
            LOG_WARN << "DnsServiceDeRegister failed: " << rc;
            return;
        }
        if (!s.cv.wait_for(lock, std::chrono::seconds(3), [&s] { return !s.pending; })) {
            LOG_WARN << "mDNS deregistration still pending";
        }
    }

    bool register_bonjour_service(const std::string& name, const std::string& regtype, uint16_t port,
                                  const std::vector<std::pair<std::string, std::string>>& txt_records) {
        TXTRecordRef txt;
        bonjour.TXTRecordCreate(&txt, 0, nullptr);
        for (const auto& kv : txt_records) {
            bonjour.TXTRecordSetValue(&txt, kv.first.c_str(), static_cast<uint8_t>(kv.second.size()), kv.second.data());
        }

        DNSServiceRef ref = nullptr;
        DNSServiceErrorType err = bonjour.DNSServiceRegister(
            &ref,
            0,
            0,
            name.c_str(),
            regtype.c_str(),
            nullptr,
            nullptr,
            htons(port),
            bonjour.TXTRecordGetLength(&txt),
            bonjour.TXTRecordGetBytesPtr(&txt),
            nullptr,
            nullptr
        );
        bonjour.TXTRecordDeallocate(&txt);

        if (err == 0 && ref) {
            bonjour_refs.push_back(ref);
            LOG_INFO << "Bonjour: registered " << name << " (" << regtype << ") on port " << port;
            return true;
        } else {
            LOG_WARN << "Bonjour: failed to register " << name << " (" << regtype << ") err=" << err;
            return false;
        }
    }

    bool start(const ap::airplay::DeviceContext& ctx, uint16_t port) {
        // Hex-encode Ed25519 public key
        std::string pk_hex;
        {
            static const char* hexd = "0123456789abcdef";
            pk_hex.reserve(ctx.public_key.size() * 2);
            for (unsigned char b : ctx.public_key) {
                pk_hex.push_back(hexd[b >> 4]);
                pk_hex.push_back(hexd[b & 0x0f]);
            }
        }

        std::string hex;
        for (char c : ctx.deviceid) if (c != ':') hex.push_back(c);
        std::string raop_name = hex + "@" + ctx.name;

        std::string cast_id;
        for (char c : ctx.pi) {
            if (c != '-') cast_id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (cast_id.size() != 32) {
            cast_id = "b08f5a79db294384b456a4784d9e6055";
        }

        // Try Apple Bonjour (dnssd.dll) first for reliable cross-platform mDNS (Android + iOS)
        if (bonjour.load()) {
            use_bonjour = true;
            LOG_INFO << "mDNS: using Apple Bonjour (dnssd.dll) service provider";

            // 1. _airplay._tcp
            register_bonjour_service(ctx.name, "_airplay._tcp", port, {
                {"deviceid", ctx.deviceid},
                {"features", ctx.features},
                {"flags",    "0x4"},
                {"model",    ctx.model},
                {"pi",       ctx.pi},
                {"pk",       pk_hex},
                {"pw",       "false"},
                {"srcvers",  ctx.srcvers},
                {"vv",       "2"}
            });

            // 2. _raop._tcp
            register_bonjour_service(raop_name, "_raop._tcp", port, {
                {"txtvers", "1"},
                {"ch",      "2"},
                {"cn",      "0,1,2,3"},
                {"da",      "true"},
                {"et",      "0,3,5"},
                {"ft",      ctx.features},
                {"md",      "0,1,2"},
                {"am",      ctx.model},
                {"rhd",     "5.6.0.0"},
                {"pk",      pk_hex},
                {"pw",      "false"},
                {"sf",      "0x4"},
                {"sr",      "44100"},
                {"ss",      "16"},
                {"sv",      "false"},
                {"tp",      "UDP"},
                {"vn",      "65537"},
                {"vs",      ctx.srcvers},
                {"vv",      "2"}
            });

            // 3. _googlecast._tcp
            register_bonjour_service(cast_id, "_googlecast._tcp", 8009, {
                {"id", cast_id},
                {"ve", "05"},
                {"md", "Chromecast"},
                {"ic", "/setup/icon.png"},
                {"fn", ctx.name},
                {"ca", "4101"},
                {"st", "0"},
                {"nf", "1"}
            });

            // 4. _display._tcp (Miracast)
            register_bonjour_service(ctx.name, "_display._tcp", 7236, {
                {"container-id", "{" + ctx.pi + "}"},
                {"friendly-name", ctx.name},
                {"wfd-version", "0100"}
            });

            return true;
        }

        // Fallback: Windows DnsServiceRegister
        LOG_INFO << "mDNS: dnssd.dll not available, falling back to Windows DnsServiceRegister";
        wchar_t host[256];
        DWORD   sz = ARRAYSIZE(host);
        if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &sz)) {
            LOG_ERROR << "GetComputerNameExW failed: " << GetLastError();
            return false;
        }
        std::wstring local_host = std::wstring(host) + L".local";
        std::wstring pk_hex_w = widen(pk_hex);

        // ---- _airplay._tcp --------------------------------------------------
        airplay->host_name     = local_host;
        airplay->instance_name = widen(ctx.name) + L"._airplay._tcp.local";
        airplay->keys          = { L"deviceid", L"features", L"flags", L"model",
                                  L"pi", L"pk", L"pw", L"srcvers", L"vv" };
        airplay->values        = { widen(ctx.deviceid), widen(ctx.features),
                                  L"0x4",             widen(ctx.model),
                                  widen(ctx.pi),      pk_hex_w,
                                  L"false",           widen(ctx.srcvers),
                                  L"2" };
        if (!register_service(airplay, port)) return false;

        // ---- _raop._tcp -----------------------------------------------------
        raop->host_name     = local_host;
        raop->instance_name = widen(raop_name) + L"._raop._tcp.local";
        raop->keys          = { L"txtvers", L"ch",  L"cn",  L"da", L"et",
                               L"ft",      L"md",  L"am",  L"rhd", L"pk",
                               L"pw",      L"sf",  L"sr",  L"ss",  L"sv",
                               L"tp",      L"vn",  L"vs",  L"vv" };
        raop->values        = { L"1",                L"2",
                               L"0,1,2,3",          L"true",
                               L"0,3,5",            widen(ctx.features),
                               L"0,1,2",            widen(ctx.model),
                               L"5.6.0.0",          pk_hex_w,
                               L"false",            L"0x4",
                               L"44100",            L"16",
                               L"false",            L"UDP",
                               L"65537",            widen(ctx.srcvers),
                               L"2" };
        if (!register_service(raop, port)) {
            LOG_WARN << "_raop._tcp registration failed; mirroring may still be offered";
        }

        // ---- _googlecast._tcp -----------------------------------------------
        googlecast->host_name     = local_host;
        googlecast->instance_name = widen(cast_id) + L"._googlecast._tcp.local";
        googlecast->keys          = { L"id", L"ve", L"md", L"ic", L"fn", L"ca", L"st", L"nf" };
        googlecast->values        = { widen(cast_id), L"05", L"Chromecast", L"/setup/icon.png",
                                     widen(ctx.name), L"4101", L"0", L"1" };
        if (!register_service(googlecast, 8009)) {
            LOG_WARN << "_googlecast._tcp registration failed; Chromecast may not be discovered";
        }

        // ---- _display._tcp (Miracast) ----------------------------------------
        miracast->host_name     = local_host;
        miracast->instance_name = widen(ctx.name) + L"._display._tcp.local";
        miracast->keys          = { L"container-id", L"friendly-name", L"wfd-version" };
        miracast->values        = { L"{" + widen(ctx.pi) + L"}", widen(ctx.name), L"0100" };
        if (!register_service(miracast, 7236)) {
            LOG_WARN << "_display._tcp registration failed; Android Miracast may not be discovered over Wi-Fi";
        }

        return true;
    }

    void stop() {
        if (use_bonjour) {
            for (auto ref : bonjour_refs) {
                if (ref && bonjour.DNSServiceRefDeallocate) {
                    bonjour.DNSServiceRefDeallocate(ref);
                }
            }
            bonjour_refs.clear();
            return;
        }

        unregister_service(airplay);
        unregister_service(raop);
        unregister_service(googlecast);
        unregister_service(miracast);
    }
};

#elif defined(HAVE_AVAHI)

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>

struct MdnsService::Impl {
    ~Impl() { stop(); }

    AvahiSimplePoll*      poll   = nullptr;
    AvahiClient*          client = nullptr;
    AvahiEntryGroup*      group  = nullptr;
    std::thread           thread;
    std::atomic<bool>     running{false};
    ap::airplay::DeviceContext ctx;
    uint16_t              port   = 0;

    static AvahiStringList* txt_list(const std::vector<std::pair<const char*, std::string>>& items) {
        AvahiStringList* list = nullptr;
        for (const auto& item : items) {
            std::string entry = std::string(item.first) + "=" + item.second;
            list = avahi_string_list_add(list, entry.c_str());
        }
        return list;
    }

    bool register_services() {
        if (!client) return false;
        if (!group) {
            group = avahi_entry_group_new(client, nullptr, nullptr);
            if (!group) {
                LOG_ERROR << "avahi_entry_group_new failed: "
                          << avahi_strerror(avahi_client_errno(client));
                return false;
            }
        }

        std::string pk;
        {
            static const char* hexd = "0123456789abcdef";
            pk.reserve(ctx.public_key.size() * 2);
            for (unsigned char b : ctx.public_key) {
                pk.push_back(hexd[b >> 4]);
                pk.push_back(hexd[b & 0x0f]);
            }
        }

        AvahiStringList* airplay_txt = txt_list({
            {"deviceid", ctx.deviceid},
            {"features", ctx.features},
            {"flags",    std::string("0x4")},
            {"model",    ctx.model},
            {"pi",       ctx.pi},
            {"pk",       pk},
            {"pw",       std::string("false")},
            {"srcvers",  ctx.srcvers},
            {"vv",       std::string("2")},
        });
        int rc = avahi_entry_group_add_service_strlst(
            group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
            AvahiPublishFlags(0),
            ctx.name.c_str(), "_airplay._tcp",
            nullptr, nullptr, port, airplay_txt);
        avahi_string_list_free(airplay_txt);
        if (rc < 0) {
            LOG_ERROR << "Avahi add _airplay._tcp failed: "
                      << avahi_strerror(rc);
            return false;
        }

        std::string hex;
        for (char c : ctx.deviceid) if (c != ':') hex.push_back(c);
        const std::string raop_name = hex + "@" + ctx.name;
        AvahiStringList* raop_txt = txt_list({
            {"txtvers", std::string("1")},
            {"ch",      std::string("2")},
            {"cn",      std::string("0,1,2,3")},
            {"da",      std::string("true")},
            {"et",      std::string("0,3,5")},
            {"ft",      ctx.features},
            {"md",      std::string("0,1,2")},
            {"am",      ctx.model},
            {"rhd",     std::string("5.6.0.0")},
            {"pk",      pk},
            {"pw",      std::string("false")},
            {"sf",      std::string("0x4")},
            {"sr",      std::string("44100")},
            {"ss",      std::string("16")},
            {"sv",      std::string("false")},
            {"tp",      std::string("UDP")},
            {"vn",      std::string("65537")},
            {"vs",      ctx.srcvers},
            {"vv",      std::string("2")},
        });
        rc = avahi_entry_group_add_service_strlst(
            group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
            AvahiPublishFlags(0),
            raop_name.c_str(), "_raop._tcp",
            nullptr, nullptr, port, raop_txt);
        avahi_string_list_free(raop_txt);
        if (rc < 0) {
            LOG_WARN << "Avahi add _raop._tcp failed: " << avahi_strerror(rc);
        }

        rc = avahi_entry_group_commit(group);
        if (rc < 0) {
            LOG_ERROR << "avahi_entry_group_commit failed: "
                      << avahi_strerror(rc);
            return false;
        }
        LOG_INFO << "Avahi registered: " << ctx.name
                 << "._airplay._tcp.local and _raop._tcp.local on port " << port;
        return true;
    }

    static void client_cb(AvahiClient* c, AvahiClientState state, void* ud) {
        auto* self = static_cast<Impl*>(ud);
        self->client = c;
        if (state == AVAHI_CLIENT_S_RUNNING) {
            self->register_services();
        } else if (state == AVAHI_CLIENT_FAILURE) {
            LOG_ERROR << "Avahi client failure: "
                      << avahi_strerror(avahi_client_errno(c));
        }
    }

    bool start(const ap::airplay::DeviceContext& ctx_in, uint16_t port_in) {
        ctx  = ctx_in;
        port = port_in;
        poll = avahi_simple_poll_new();
        if (!poll) { LOG_ERROR << "avahi_simple_poll_new failed"; return false; }
        int err = 0;
        client = avahi_client_new(avahi_simple_poll_get(poll),
                                  AvahiClientFlags(0),
                                  &Impl::client_cb, this, &err);
        if (!client) {
            LOG_ERROR << "avahi_client_new failed: " << avahi_strerror(err);
            avahi_simple_poll_free(poll); poll = nullptr;
            return false;
        }
        running = true;
        thread = std::thread([this]() {
            avahi_simple_poll_loop(poll);
        });
        return true;
    }

    void stop() {
        if (!running.exchange(false)) return;
        if (poll)   avahi_simple_poll_quit(poll);
        if (thread.joinable()) thread.join();
        if (client) avahi_client_free(client);
        if (poll)   avahi_simple_poll_free(poll);
        client = nullptr; poll = nullptr; group = nullptr;
    }
};

#else // non-Windows without Avahi: stub

struct MdnsService::Impl {
    bool start(const ap::airplay::DeviceContext&, uint16_t) {
        LOG_WARN << "mDNS advertising unavailable in this build "
                    "(install libavahi-client-dev + reconfigure to enable on Linux)";
        return false;
    }
    void stop() {}
};

#endif

MdnsService::MdnsService() : impl_(std::make_unique<Impl>()) {}
MdnsService::~MdnsService() = default;

bool MdnsService::start(const ap::airplay::DeviceContext& ctx, uint16_t port) {
    return impl_->start(ctx, port);
}

void MdnsService::stop() {
    impl_->stop();
}

} // namespace ap::mdns
