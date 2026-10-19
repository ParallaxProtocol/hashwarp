#define LOG_SOURCE "rigs"

// Hashwarp multi-rig view: polls the API of other rigs for the dashboard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#include "Rigs.h"

#include <libdevcore/Log.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

using boost::asio::ip::tcp;

namespace hashwarp
{
namespace api
{
namespace
{
const uint16_t c_defaultPort = 21550;
const size_t c_maxResponse = 1 << 20;  // a summary is a few kB

// One GET, alive until its handler ran
class HttpGet : public std::enable_shared_from_this<HttpGet>
{
public:
    HttpGet(boost::asio::io_service& _io, HttpGetHandler _handler)
      : m_resolver(_io), m_socket(_io), m_timer(_io), m_handler(std::move(_handler))
    {}

    void start(const std::string& _host, uint16_t _port, const std::string& _path,
        const std::string& _token, std::chrono::milliseconds _timeout)
    {
        std::ostringstream req;
        req << "GET " << _path << " HTTP/1.1\r\nHost: " << _host << ":" << _port
            << "\r\nAccept: application/json\r\nUser-Agent: hashwarp\r\n";
        if (!_token.empty())
            req << "Authorization: Bearer " << _token << "\r\n";
        req << "Connection: close\r\n\r\n";
        m_request = req.str();

        auto self = shared_from_this();
        m_timer.expires_after(_timeout);
        m_timer.async_wait([self, _timeout](const boost::system::error_code& _ec) {
            if (!_ec)
                self->finish(
                    "no answer in " + std::to_string(_timeout.count() / 1000.0).substr(0, 3) + " s");
        });
        m_resolver.async_resolve(_host, std::to_string(_port),
            [self](const boost::system::error_code& _ec, tcp::resolver::results_type _results) {
                if (_ec)
                    return self->finish(_ec.message());
                boost::asio::async_connect(self->m_socket, _results,
                    [self](const boost::system::error_code& _ec, const tcp::endpoint&) {
                        if (_ec)
                            return self->finish(_ec.message());
                        boost::asio::async_write(self->m_socket, boost::asio::buffer(self->m_request),
                            [self](const boost::system::error_code& _ec, size_t) {
                                if (_ec)
                                    return self->finish(_ec.message());
                                self->read();
                            });
                    });
            });
    }

private:
    void read()
    {
        auto self = shared_from_this();
        m_socket.async_read_some(boost::asio::buffer(m_buffer),
            [self](const boost::system::error_code& _ec, size_t _n) {
                self->m_response.append(self->m_buffer.data(), _n);
                if (self->m_response.size() > c_maxResponse)
                    return self->finish("the answer is too large");
                if (_ec == boost::asio::error::eof || (!_ec && self->complete()))
                    return self->parse();
                if (_ec)
                    return self->finish(_ec.message());
                self->read();
            });
    }

    // Whether the headers and Content-Length bytes of body arrived
    bool complete() const
    {
        size_t end = m_response.find("\r\n\r\n");
        if (end == std::string::npos)
            return false;
        std::string head = m_response.substr(0, end);
        for (char& c : head)
            c = (char)std::tolower((unsigned char)c);
        size_t cl = head.find("\r\ncontent-length:");
        if (cl == std::string::npos)
            return false;  // until the server closes the connection
        size_t length = std::strtoul(head.c_str() + cl + 17, nullptr, 10);
        return m_response.size() >= end + 4 + length;
    }

    void parse()
    {
        int status = 0;
        size_t end = m_response.find("\r\n\r\n");
        if (end == std::string::npos || m_response.compare(0, 5, "HTTP/") != 0 ||
            sscanf(m_response.c_str(), "HTTP/%*s %d", &status) != 1)
            return finish("not an HTTP answer");
        finish("", status, m_response.substr(end + 4));
    }

    void finish(const std::string& _error, int _status = 0, const std::string& _body = "")
    {
        if (m_done)
            return;
        m_done = true;
        boost::system::error_code ec;
        m_timer.cancel();
        m_resolver.cancel();
        m_socket.close(ec);
        m_handler(_error, _status, _body);
    }

    tcp::resolver m_resolver;
    tcp::socket m_socket;
    boost::asio::steady_timer m_timer;
    HttpGetHandler m_handler;
    std::string m_request, m_response;
    std::array<char, 8192> m_buffer;
    bool m_done = false;
};
}  // namespace

RigAddress parseRig(const std::string& _spec)
{
    RigAddress r;
    std::string s = _spec;
    size_t eq = s.find('=');
    if (eq != std::string::npos && s.substr(0, eq).find_first_of("@:/") == std::string::npos)
    {
        r.name = s.substr(0, eq);
        s = s.substr(eq + 1);
    }
    if (s.compare(0, 8, "https://") == 0)
        throw std::invalid_argument("--rig " + _spec + ": the API is plain http");
    if (s.compare(0, 7, "http://") == 0)
        s = s.substr(7);
    if (!s.empty() && s.back() == '/')
        s.pop_back();
    size_t at = s.rfind('@');
    if (at != std::string::npos)
    {
        r.token = s.substr(0, at);
        s = s.substr(at + 1);
    }

    // host, host:port, [v6], [v6]:port
    std::string port;
    if (!s.empty() && s[0] == '[')
    {
        size_t close = s.find(']');
        if (close == std::string::npos)
            throw std::invalid_argument("--rig " + _spec + ": missing ]");
        r.host = s.substr(1, close - 1);
        if (close + 1 < s.size())
        {
            if (s[close + 1] != ':')
                throw std::invalid_argument("--rig " + _spec + ": expected :port after ]");
            port = s.substr(close + 2);
        }
    }
    else
    {
        size_t colon = s.rfind(':');
        r.host = s.substr(0, colon);
        if (colon != std::string::npos)
            port = s.substr(colon + 1);
    }
    if (r.host.empty() || r.host.find_first_of("/ ") != std::string::npos)
        throw std::invalid_argument(
            "--rig " + _spec + ": use [name=][token@]host[:port], e.g. rig2=10.0.0.12:21550");
    r.port = c_defaultPort;
    if (!port.empty())
    {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos ||
            std::stoul(port) == 0 || std::stoul(port) > 65535)
            throw std::invalid_argument("--rig " + _spec + ": invalid port " + port);
        r.port = (uint16_t)std::stoul(port);
    }
    if (r.name.empty())
        r.name = r.host;
    return r;
}

void httpGet(boost::asio::io_service& _io, const std::string& _host, uint16_t _port,
    const std::string& _path, const std::string& _token, std::chrono::milliseconds _timeout,
    HttpGetHandler _handler)
{
    std::make_shared<HttpGet>(_io, std::move(_handler))->start(_host, _port, _path, _token, _timeout);
}

RigPoller::RigPoller(
    boost::asio::io_service& _io, std::vector<RigAddress> _rigs, std::chrono::seconds _interval)
  : m_io(_io),
    m_interval(_interval),
    m_timer(std::make_shared<boost::asio::steady_timer>(_io)),
    m_alive(std::make_shared<std::atomic<bool>>(true))
{
    for (auto& a : _rigs)
    {
        Rig r;
        r.address = std::move(a);
        m_rigs.push_back(std::move(r));
    }
}

RigPoller::~RigPoller()
{
    stop();
}

void RigPoller::start()
{
    auto alive = m_alive;
    m_io.post([this, alive]() {
        if (*alive)
            poll();
    });
}

void RigPoller::stop()
{
    // Handlers still queued check m_alive, and only they touch this
    if (!m_alive->exchange(false))
        return;
    auto timer = m_timer;
    m_io.post([timer]() { timer->cancel(); });
}

void RigPoller::poll()
{
    auto alive = m_alive;
    {
        std::lock_guard<std::mutex> l(x_rigs);
        for (size_t i = 0; i < m_rigs.size(); i++)
        {
            Rig& r = m_rigs[i];
            if (r.polling)
                continue;
            r.polling = true;
            httpGet(m_io, r.address.host, r.address.port, "/api/v1/summary", r.address.token,
                std::chrono::milliseconds(4000),
                [this, alive, i](const std::string& _error, int _status, const std::string& _body) {
                    if (*alive)
                        received(i, _error, _status, _body);
                });
        }
    }
    auto timer = m_timer;
    timer->expires_after(m_interval);
    timer->async_wait([this, alive, timer](const boost::system::error_code& _ec) {
        if (!_ec && *alive)
            poll();
    });
}

void RigPoller::received(
    size_t _index, const std::string& _error, int _status, const std::string& _body)
{
    std::string error = _error;
    Json::Value summary;
    if (error.empty())
    {
        if (_status == 401)
            error = "wrong or missing token";
        else if (_status == 404)
            error = "no /api/v1/summary: older than hashwarp 2.0, or not mining (--monitor)";
        else if (_status != 200)
            error = "HTTP " + std::to_string(_status);
        else
        {
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream in(_body);
            if (!Json::parseFromStream(builder, in, &summary, &errors) || !summary.isObject() ||
                !summary.isMember("miner") || !summary.isMember("gpus"))
                error = "not a hashwarp API";
        }
    }

    std::lock_guard<std::mutex> l(x_rigs);
    Rig& r = m_rigs.at(_index);
    r.polling = false;
    bool changed = !r.polled || r.online != error.empty();
    r.polled = true;
    r.online = error.empty();
    r.error = error;
    if (changed && r.online)
        cnote << r.address.name << " online";
    else if (changed)
        cwarn << r.address.name << " (" << r.address.host << ":" << r.address.port
              << ") unreachable: " << error;
    if (r.online)
    {
        r.summary = summary;
        r.lastOk = std::chrono::steady_clock::now();
    }
}

Json::Value RigPoller::rigs() const
{
    Json::Value ret(Json::arrayValue);
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(x_rigs);
    for (const auto& r : m_rigs)
    {
        Json::Value j;
        j["name"] = r.address.name;
        j["host"] = r.address.host + ":" + std::to_string(r.address.port);
        j["online"] = r.online;
        j["error"] = r.error;
        j["age"] = r.summary.isNull() ?
                       -1 :
                       Json::Int64(
                           std::chrono::duration_cast<std::chrono::seconds>(now - r.lastOk).count());
        if (!r.summary.isNull())
            j["summary"] = r.summary;
        ret.append(j);
    }
    return ret;
}

}  // namespace api
}  // namespace hashwarp
