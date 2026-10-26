/*
===========================================================================
Copyright (C) 2000-2013 Darklegion Development
Copyright (C) 2015-2019 GrangerHub

This file is part of Tremulous.

Tremulous is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 3 of the License,
or (at your option) any later version.

Tremulous is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Tremulous; if not, see <https://www.gnu.org/licenses/>

===========================================================================
*/

// WebSocket listener for native dedicated servers. The web client's sockets
// are emscripten's emulated UDP sockets, which send each datagram as one
// binary websocket message, so every message is handed to the server as a
// packet from the browser's address and TCP port, and every packet sent back
// to that address goes out as one message.
//
// websocketpp runs asio on its own thread: received packets are queued for
// the main thread, which a pipe in NET_Sleep's select() wakes, and packets
// for the browsers are posted back to the websocket thread.
//
// The same port answers plain HTTP GETs for the paks clients may download
// (net_wsDownload), so the web client can fetch a server's maps from the
// server itself. The main thread decides which file a request gets, and the
// websocket thread reads and sends it.

#include "net_ws.h"

#ifdef USE_WEBSOCKET

#define ASIO_STANDALONE
#define _WEBSOCKETPP_CPP11_STL_

#ifdef USE_WEBSOCKET_TLS
#include <websocketpp/config/asio.hpp>
#else
#include <websocketpp/config/asio_no_tls.hpp>
#endif
#include <websocketpp/server.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "cvar.h"
#include "files.h"
#include "msg.h"
#include "q_shared.h"
#include "qcommon.h"

static cvar_t *net_websocket;
static cvar_t *net_wsPort;
static cvar_t *net_wsCert;
static cvar_t *net_wsKey;
static cvar_t *net_wsDownload;

namespace
{
const int WS_MAX_CONNS = 256;
const int WS_MAX_PER_IP = 16;
const int WS_IDLE_TIMEOUT = 60;  // seconds without a message from the browser
const size_t WS_MAX_BUFFERED = 256 * 1024;  // drop packets for browsers that can't keep up
const int WS_MAX_HTTP = 32;  // HTTP requests waiting for the main thread

typedef websocketpp::connection_hdl Hdl;
typedef std::chrono::steady_clock Clock;

enum EventType { EV_PACKET, EV_OPEN, EV_CLOSE, EV_HTTP };

// handed from the websocket thread to the main thread
struct Event {
    EventType type;
    netadr_t adr;
    std::string data;  // the packet, or the path an HTTP request asks for
    Hdl hdl;  // the HTTP request's connection
};

std::string BaseAdrKey(const netadr_t &a)
{
    std::string key(1, (char)a.type);

    if (a.type == NA_IP)
        key.append((const char *)a.ip, sizeof(a.ip));
    else
        key.append((const char *)a.ip6, sizeof(a.ip6));

    return key;
}

std::string AdrKey(const netadr_t &a)
{
    return BaseAdrKey(a).append((const char *)&a.port, sizeof(a.port));
}

void AddressToAdr(asio::ip::address addr, unsigned short port, netadr_t *a)
{
    memset(a, 0, sizeof(*a));

    if (addr.is_v6() && addr.to_v6().is_v4_mapped())
        addr = asio::ip::make_address_v4(asio::ip::v4_mapped, addr.to_v6());

    if (addr.is_v4())
    {
        asio::ip::address_v4::bytes_type b = addr.to_v4().to_bytes();

        a->type = NA_IP;
        memcpy(a->ip, b.data(), sizeof(a->ip));
    }
    else
    {
        asio::ip::address_v6::bytes_type b = addr.to_v6().to_bytes();

        a->type = NA_IP6;
        memcpy(a->ip6, b.data(), sizeof(a->ip6));
        a->scope_id = addr.to_v6().scope_id();
    }

    a->port = htons(port);
    a->alternateProtocol = 0;
}

bool IsLoopback(asio::ip::address addr)
{
    if (addr.is_v6() && addr.to_v6().is_v4_mapped())
        addr = asio::ip::make_address_v4(asio::ip::v4_mapped, addr.to_v6());

    return addr.is_loopback();
}

// what the main thread sees, whether the server speaks ws:// or wss://
class Server
{
   public:
    Server()
    {
        if (pipe(wakeFds) == 0)
        {
            fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
            fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
        }
        else
            wakeFds[0] = wakeFds[1] = -1;
    }

    virtual ~Server()
    {
        if (wakeFds[0] != -1)
        {
            close(wakeFds[0]);
            close(wakeFds[1]);
        }
    }

    virtual bool Start(const asio::ip::tcp::endpoint &ep, std::string &err) = 0;
    virtual void Stop() = 0;
    virtual bool Send(const netadr_t &to, const void *data, int length) = 0;

    // answers an HTTP request with the file at ospath, or with status when
    // ospath is empty
    virtual void Respond(Hdl hdl, const std::string &ospath, int status) = 0;

    bool serveFiles = false;

    int WakeFd() const { return wakeFds[0]; }

    void Drain()
    {
        char buf[256];

        while (read(wakeFds[0], buf, sizeof(buf)) > 0)
            ;
    }

    bool Pop(Event &ev)
    {
        std::lock_guard<std::mutex> guard(lock);

        if (events.empty()) return false;

        ev = std::move(events.front());
        events.pop_front();
        return true;
    }

   protected:
    void Push(EventType type, const netadr_t &adr, std::string data = std::string(), Hdl hdl = Hdl())
    {
        {
            std::lock_guard<std::mutex> guard(lock);

            events.push_back(Event{type, adr, std::move(data), hdl});
        }

        if (write(wakeFds[1], "", 1) < 0)
        {
            // the pipe is already full of wakeups
        }
    }

    std::mutex lock;  // guards events and the address map below

   private:
    std::deque<Event> events;
    int wakeFds[2];
};

template <typename Config>
class ServerImpl : public Server
{
   public:
    typedef websocketpp::server<Config> Endpoint;

    ServerImpl() : open(0) {}
    ~ServerImpl() { Stop(); }

    Endpoint &Get() { return endpoint; }

    bool Start(const asio::ip::tcp::endpoint &ep, std::string &err) override
    {
        using websocketpp::lib::placeholders::_1;
        using websocketpp::lib::placeholders::_2;
        websocketpp::lib::error_code ec;

        endpoint.clear_access_channels(websocketpp::log::alevel::all);
        endpoint.clear_error_channels(websocketpp::log::elevel::all);

        endpoint.init_asio(ec);
        if (ec)
        {
            err = ec.message();
            return false;
        }

        endpoint.set_reuse_addr(true);
        endpoint.set_max_message_size(MAX_MSGLEN);
        endpoint.set_close_handshake_timeout(1000);

        endpoint.set_validate_handler(websocketpp::lib::bind(&ServerImpl::OnValidate, this, _1));
        endpoint.set_open_handler(websocketpp::lib::bind(&ServerImpl::OnOpen, this, _1));
        endpoint.set_close_handler(websocketpp::lib::bind(&ServerImpl::OnClose, this, _1));
        endpoint.set_fail_handler(websocketpp::lib::bind(&ServerImpl::OnClose, this, _1));
        endpoint.set_message_handler(websocketpp::lib::bind(&ServerImpl::OnMessage, this, _1, _2));
        endpoint.set_http_handler(websocketpp::lib::bind(&ServerImpl::OnHttp, this, _1));

        endpoint.listen(ep, ec);
        if (ec)
        {
            err = ec.message();
            return false;
        }

        endpoint.start_accept(ec);
        if (ec)
        {
            err = ec.message();
            return false;
        }

        idleTimer.reset(new asio::steady_timer(endpoint.get_io_service()));
        CheckIdle();

        thread = std::thread([this]() {
            for (;;)
            {
                try
                {
                    endpoint.run();
                    break;
                }
                catch (const std::exception &)
                {
                    // one bad connection shouldn't take the others down
                }
            }
        });

        return true;
    }

    void Stop() override
    {
        if (!thread.joinable()) return;

        asio::post(endpoint.get_io_service(), [this]() {
            websocketpp::lib::error_code ec;

            endpoint.stop_listening(ec);
            idleTimer->cancel();

            for (auto &c : conns)
                endpoint.close(c.first, websocketpp::close::status::going_away, "server shutting down", ec);
        });

        // give the browsers a moment to hear about it
        for (int i = 0; i < 100 && open > 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        endpoint.stop();
        thread.join();
    }

    bool Send(const netadr_t &to, const void *data, int length) override
    {
        Hdl hdl;

        {
            std::lock_guard<std::mutex> guard(lock);
            auto it = byAdr.find(AdrKey(to));

            if (it == byAdr.end()) return false;

            hdl = it->second;
        }

        std::string payload((const char *)data, length);

        asio::post(endpoint.get_io_service(), [this, hdl, payload]() {
            websocketpp::lib::error_code ec;
            typename Endpoint::connection_ptr con = endpoint.get_con_from_hdl(hdl, ec);

            if (ec || con->get_buffered_amount() > WS_MAX_BUFFERED) return;

            con->send(payload, websocketpp::frame::opcode::binary);
        });

        return true;
    }

    void Respond(Hdl hdl, const std::string &ospath, int status) override
    {
        asio::post(endpoint.get_io_service(), [this, hdl, ospath, status]() {
            websocketpp::lib::error_code ec;
            auto it = deferred.find(hdl);

            if (it == deferred.end()) return;

            typename Endpoint::connection_ptr con = it->second;

            deferred.erase(it);

            std::ifstream file;

            if (!ospath.empty()) file.open(ospath, std::ios::binary);

            if (file.is_open())
            {
                std::ostringstream body;

                body << file.rdbuf();
                con->set_status(websocketpp::http::status_code::ok);
                con->set_body(body.str());
                con->replace_header("Content-Type", "application/octet-stream");
            }
            else
            {
                con->set_status(ospath.empty() ? (websocketpp::http::status_code::value)status
                                               : websocketpp::http::status_code::not_found);
                con->set_body("");
            }

            con->send_http_response(ec);
        });
    }

   private:
    struct Conn {
        netadr_t adr;
        bool first;
        Clock::time_point lastRecv;
    };

    bool OnValidate(Hdl hdl)
    {
        websocketpp::lib::error_code ec;
        asio::error_code aec;
        typename Endpoint::connection_ptr con = endpoint.get_con_from_hdl(hdl, ec);

        if (ec) return false;

        asio::ip::tcp::endpoint remote = con->get_raw_socket().remote_endpoint(aec);

        if (aec) return false;

        netadr_t adr;

        AddressToAdr(remote.address(), remote.port(), &adr);

        // behind a reverse proxy on this machine, the browser's own address
        // comes in X-Forwarded-For
        if (IsLoopback(remote.address()))
        {
            std::string fwd = con->get_request_header("X-Forwarded-For");
            size_t end = fwd.find(',');

            fwd = fwd.substr(0, end);
            fwd.erase(0, fwd.find_first_not_of(" \t"));
            fwd.erase(fwd.find_last_not_of(" \t") + 1);

            if (!fwd.empty())
            {
                asio::ip::address real = asio::ip::make_address(fwd, aec);

                if (!aec) AddressToAdr(real, remote.port(), &adr);
            }
        }

        if (conns.size() >= (size_t)WS_MAX_CONNS)
        {
            con->set_status(websocketpp::http::status_code::service_unavailable);
            return false;
        }

        std::string base = BaseAdrKey(adr);
        int sameIP = 0;

        for (auto &c : conns)
        {
            if (BaseAdrKey(c.second.adr) == base) sameIP++;
        }

        if (sameIP >= WS_MAX_PER_IP)
        {
            con->set_status(websocketpp::http::status_code::too_many_requests);
            return false;
        }

        // emscripten asks for the "binary" subprotocol, and browsers drop
        // the connection if the server doesn't agree to it
        for (const std::string &p : con->get_requested_subprotocols())
        {
            if (p == "binary")
            {
                con->select_subprotocol(p, ec);
                break;
            }
        }

        conns[hdl] = Conn{adr, true, Clock::now()};
        return true;
    }

    void OnOpen(Hdl hdl)
    {
        auto it = conns.find(hdl);

        if (it == conns.end()) return;

        {
            std::lock_guard<std::mutex> guard(lock);

            byAdr[AdrKey(it->second.adr)] = hdl;
        }

        open++;
        Push(EV_OPEN, it->second.adr);
    }

    void OnClose(Hdl hdl)
    {
        auto it = conns.find(hdl);

        if (it == conns.end()) return;

        bool wasOpen = false;

        {
            std::lock_guard<std::mutex> guard(lock);
            auto b = byAdr.find(AdrKey(it->second.adr));

            if (b != byAdr.end() && !b->second.owner_before(hdl) && !hdl.owner_before(b->second))
            {
                byAdr.erase(b);
                wasOpen = true;
            }
        }

        if (wasOpen)
        {
            open--;
            Push(EV_CLOSE, it->second.adr);
        }

        conns.erase(it);
    }

    void OnMessage(Hdl hdl, typename Endpoint::message_ptr msg)
    {
        auto it = conns.find(hdl);

        if (it == conns.end()) return;

        Conn &c = it->second;
        bool first = c.first;

        c.lastRecv = Clock::now();
        c.first = false;

        if (msg->get_opcode() != websocketpp::frame::opcode::binary) return;

        std::string &payload = msg->get_raw_payload();

        // emscripten's emulated UDP sockets may open with a note of their
        // local port, which means nothing here
        if (first && payload.size() == 10 && !memcmp(payload.data(), "\xff\xff\xff\xffport", 8)) return;

        Push(EV_PACKET, c.adr, std::move(payload));
    }

    void OnHttp(Hdl hdl)
    {
        websocketpp::lib::error_code ec;
        asio::error_code aec;
        typename Endpoint::connection_ptr con = endpoint.get_con_from_hdl(hdl, ec);

        if (ec) return;

        // the web client is served from another origin
        con->append_header("Access-Control-Allow-Origin", "*");

        if (!serveFiles)
        {
            con->set_status(websocketpp::http::status_code::not_found);
            return;
        }

        if (con->get_request().get_method() != "GET")
        {
            con->set_status(websocketpp::http::status_code::method_not_allowed);
            return;
        }

        if (deferred.size() >= (size_t)WS_MAX_HTTP)
        {
            con->set_status(websocketpp::http::status_code::service_unavailable);
            return;
        }

        asio::ip::tcp::endpoint remote = con->get_raw_socket().remote_endpoint(aec);
        netadr_t adr;

        if (aec) return;

        AddressToAdr(remote.address(), remote.port(), &adr);

        if (con->defer_http_response()) return;

        deferred[hdl] = con;
        Push(EV_HTTP, adr, con->get_resource(), hdl);
    }

    // closes the connections of browsers that went quiet, such as ones that
    // only asked for the server's info from the server browser
    void CheckIdle()
    {
        idleTimer->expires_after(std::chrono::seconds(10));
        idleTimer->async_wait([this](const asio::error_code &ec) {
            if (ec) return;

            Clock::time_point now = Clock::now();
            websocketpp::lib::error_code wec;

            for (auto &c : conns)
            {
                if (now - c.second.lastRecv > std::chrono::seconds(WS_IDLE_TIMEOUT))
                    endpoint.close(c.first, websocketpp::close::status::going_away, "idle", wec);
            }

            CheckIdle();
        });
    }

    Endpoint endpoint;
    std::thread thread;
    std::unique_ptr<asio::steady_timer> idleTimer;
    std::atomic<int> open;

    // only touched on the websocket thread
    std::map<Hdl, Conn, std::owner_less<Hdl>> conns;

    // HTTP requests waiting for the main thread, which nothing else keeps
    // alive while they wait
    std::map<Hdl, typename Endpoint::connection_ptr, std::owner_less<Hdl>> deferred;

    // guarded by lock, so the main thread can tell which addresses are browsers
    std::map<std::string, Hdl> byAdr;
};

#ifdef USE_WEBSOCKET_TLS
typedef std::shared_ptr<asio::ssl::context> ContextPtr;

ContextPtr TLSContext(const char *cert, const char *key, std::string &err)
{
    ContextPtr ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    asio::error_code ec;

    ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1,
        ec);

    ctx->use_certificate_chain_file(cert, ec);
    if (ec)
    {
        err = std::string(cert) + ": " + ec.message();
        return nullptr;
    }

    ctx->use_private_key_file(key, asio::ssl::context::pem, ec);
    if (ec)
    {
        err = std::string(key) + ": " + ec.message();
        return nullptr;
    }

    return ctx;
}
#endif
}  // namespace

static Server *ws_server;

/*
====================
NET_WS_DownloadPath

The file an HTTP request for resource may have, or "" with the status to send
instead. Like UDP downloads, only paks the server references can be had.
====================
*/
static std::string NET_WS_DownloadPath(std::string resource, int *status)
{
    int allow = Cvar_VariableIntegerValue("sv_allowDownload");
    size_t query = resource.find_first_of("?#");

    *status = 404;

    if (query != std::string::npos) resource.erase(query);

    while (!resource.empty() && resource[0] == '/')
        resource.erase(0, 1);

    if (!(allow & DLF_ENABLE) || (allow & DLF_NO_REDIRECT))
    {
        *status = 403;
        return "";
    }

    if (resource.size() <= 4 || resource.size() >= MAX_QPATH || Q_stricmp(resource.c_str() + resource.size() - 4, ".pk3") ||
        resource.find("..") != std::string::npos || resource.find_first_of("\\:%") != std::string::npos)
        return "";

    std::string pak = resource.substr(0, resource.size() - 4);
    std::istringstream referenced(std::string(Cvar_VariableString("sv_referencedPakNames")) + " " +
                                  Cvar_VariableString("sv_referencedAlternatePakNames"));
    std::string name;
    bool found = false;

    while (referenced >> name)
    {
        if (!FS_FilenameCompare(name.c_str(), pak.c_str()))
        {
            found = true;
            break;
        }
    }

    if (!found) return "";

    const char *bases[] = {Cvar_VariableString("fs_homepath"), Cvar_VariableString("fs_basepath")};

    for (const char *base : bases)
    {
        std::string ospath = FS_BuildOSPath(base, resource.c_str(), "");

        // remove the trailing slash
        ospath.erase(ospath.size() - 1);

        if (access(ospath.c_str(), R_OK) == 0) return ospath;
    }

    return "";
}

/*
====================
NET_WS_GetCvars
====================
*/
bool NET_WS_GetCvars(void)
{
    int modified = 0;

    // accept the web client's websocket connections directly, without websockify
    net_websocket = Cvar_Get("net_websocket", "0", CVAR_LATCH);
    modified += net_websocket->modified;
    net_websocket->modified = false;

    // 0 takes the same port number as net_port
    net_wsPort = Cvar_Get("net_wsPort", "0", CVAR_LATCH);
    modified += net_wsPort->modified;
    net_wsPort->modified = false;

    // PEM certificate chain and private key, for wss://
    net_wsCert = Cvar_Get("net_wsCert", "", CVAR_LATCH);
    modified += net_wsCert->modified;
    net_wsCert->modified = false;

    net_wsKey = Cvar_Get("net_wsKey", "", CVAR_LATCH);
    modified += net_wsKey->modified;
    net_wsKey->modified = false;

    // serve the paks clients may download over HTTP(S) on the same port, so
    // web clients download from this server rather than sv_dlURL
    net_wsDownload = Cvar_Get("net_wsDownload", "1", CVAR_LATCH);
    modified += net_wsDownload->modified;
    net_wsDownload->modified = false;

    return modified ? true : false;
}

/*
====================
NET_WS_Open
====================
*/
void NET_WS_Open(const char *ip, const char *ip6, int enabled, int udpPort)
{
    asio::error_code ec;
    asio::ip::address addr;
    std::string err;
    Server *server;
    bool tls;
    int port;

    if (ws_server || !net_websocket || !net_websocket->integer) return;

    port = net_wsPort->integer ? net_wsPort->integer : udpPort;
    tls = net_wsCert->string[0] || net_wsKey->string[0];

    // an IPv6 listener takes IPv4 connections too
    addr = asio::ip::make_address((enabled & NET_ENABLEV6) ? ip6 : ip, ec);
    if (ec)
    {
        Com_Printf("WARNING: NET_WS_Open: bad address %s\n", (enabled & NET_ENABLEV6) ? ip6 : ip);
        return;
    }

    if (tls)
    {
#ifdef USE_WEBSOCKET_TLS
        if (!net_wsCert->string[0] || !net_wsKey->string[0])
        {
            Com_Printf("WARNING: NET_WS_Open: wss:// needs both net_wsCert and net_wsKey\n");
            return;
        }

        ContextPtr ctx = TLSContext(net_wsCert->string, net_wsKey->string, err);

        if (!ctx)
        {
            Com_Printf("WARNING: NET_WS_Open: %s\n", err.c_str());
            return;
        }

        ServerImpl<websocketpp::config::asio_tls> *impl = new ServerImpl<websocketpp::config::asio_tls>();

        impl->Get().set_tls_init_handler([ctx](Hdl) { return ctx; });
        server = impl;
#else
        Com_Printf("WARNING: NET_WS_Open: net_wsCert and net_wsKey are set, but this server was built "
                   "without TLS support (USE_WEBSOCKET_TLS=0)\n");
        return;
#endif
    }
    else
        server = new ServerImpl<websocketpp::config::asio>();

    server->serveFiles = net_wsDownload->integer ? true : false;

    if (server->WakeFd() == -1 || !server->Start(asio::ip::tcp::endpoint(addr, port), err))
    {
        Com_Printf("WARNING: NET_WS_Open: couldn't listen on %s port %d: %s\n", addr.to_string().c_str(), port,
            err.c_str());
        delete server;
        return;
    }

    Com_Printf("Opening websocket listener (%s): %s:%d\n", tls ? "wss" : "ws", addr.to_string().c_str(), port);
    ws_server = server;

    // tells the web clients to download from here
    Cvar_Get("sv_wsDownload", "0", CVAR_SERVERINFO | CVAR_ROM);
    Cvar_Set2("sv_wsDownload", server->serveFiles ? "1" : "0", true);
}

/*
====================
NET_WS_Close
====================
*/
void NET_WS_Close(void)
{
    delete ws_server;
    ws_server = NULL;

    if (Cvar_VariableIntegerValue("sv_wsDownload")) Cvar_Set2("sv_wsDownload", "0", true);
}

/*
====================
NET_WS_FillFdSet
====================
*/
int NET_WS_FillFdSet(fd_set *fdr, int highestfd)
{
    if (!ws_server) return highestfd;

    FD_SET(ws_server->WakeFd(), fdr);

    return ws_server->WakeFd() > highestfd ? ws_server->WakeFd() : highestfd;
}

/*
====================
NET_WS_Frame
====================
*/
void NET_WS_Frame(fd_set *fdr)
{
    if (ws_server && FD_ISSET(ws_server->WakeFd(), fdr)) ws_server->Drain();
}

/*
====================
NET_WS_GetPacket
====================
*/
bool NET_WS_GetPacket(netadr_t *from, msg_t *msg)
{
    Event ev;

    if (!ws_server) return false;

    while (ws_server->Pop(ev))
    {
        if (ev.type == EV_OPEN)
        {
            Com_DPrintf("websocket: %s connected\n", NET_AdrToStringwPort(ev.adr));
            continue;
        }

        if (ev.type == EV_CLOSE)
        {
            Com_DPrintf("websocket: %s disconnected\n", NET_AdrToStringwPort(ev.adr));
            continue;
        }

        if (ev.type == EV_HTTP)
        {
            int status;
            std::string ospath = NET_WS_DownloadPath(ev.data, &status);

            if (!ospath.empty())
                Com_Printf("websocket: %s downloading %s\n", NET_AdrToString(ev.adr), ev.data.c_str());
            else
                Com_DPrintf("websocket: %s can't download %s (%d)\n", NET_AdrToString(ev.adr), ev.data.c_str(), status);

            ws_server->Respond(ev.hdl, ospath, status);
            continue;
        }

        if ((int)ev.data.size() >= msg->maxsize)
        {
            Com_Printf("Oversize packet from %s\n", NET_AdrToString(ev.adr));
            continue;
        }

        memcpy(msg->data, ev.data.data(), ev.data.size());
        msg->cursize = ev.data.size();
        msg->readcount = 0;
        *from = ev.adr;
        return true;
    }

    return false;
}

/*
====================
NET_WS_SendPacket

Returns false when the address isn't a browser's
====================
*/
bool NET_WS_SendPacket(int length, const void *data, const netadr_t *to)
{
    if (!ws_server || (to->type != NA_IP && to->type != NA_IP6)) return false;

    return ws_server->Send(*to, data, length);
}

#endif
