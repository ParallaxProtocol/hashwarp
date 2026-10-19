// Hashwarp multi-rig view: polls the API of other rigs for the dashboard.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <boost/asio.hpp>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hashwarp
{
namespace api
{
/// Another rig's API
struct RigAddress
{
    std::string name;  // shown on the dashboard, the host by default
    std::string host;
    uint16_t port = 0;
    std::string token;  // its --api-token, if any
};

/// Parses [name=][http://][token@]host:port. Throws std::invalid_argument
RigAddress parseRig(const std::string& _spec);

/// Result of an HTTP GET: _error is empty on success, then _status and _body are set
using HttpGetHandler =
    std::function<void(const std::string& _error, int _status, const std::string& _body)>;

/// GETs http://_host:_port_path (with the bearer _token, if any) and calls _handler on _io's
/// thread, after _timeout at the latest
void httpGet(boost::asio::io_service& _io, const std::string& _host, uint16_t _port,
    const std::string& _path, const std::string& _token, std::chrono::milliseconds _timeout,
    HttpGetHandler _handler);

/// Reads the /api/v1/summary of each rig every _interval
class RigPoller
{
public:
    RigPoller(boost::asio::io_service& _io, std::vector<RigAddress> _rigs,
        std::chrono::seconds _interval = std::chrono::seconds(5));
    ~RigPoller();

    void start();
    void stop();

    /// One object per rig: name, host, online, error, age (seconds since its last
    /// summary, -1 if none yet) and summary (the last one)
    Json::Value rigs() const;

private:
    struct Rig
    {
        RigAddress address;
        Json::Value summary;
        std::string error = "not polled yet";
        bool online = false;
        bool polling = false;
        bool polled = false;  // an answer (or failure) arrived
        std::chrono::steady_clock::time_point lastOk;
    };

    void poll();
    void received(size_t _index, const std::string& _error, int _status, const std::string& _body);

    boost::asio::io_service& m_io;
    std::chrono::seconds m_interval;
    std::shared_ptr<boost::asio::steady_timer> m_timer;  // shared with its handler
    mutable std::mutex x_rigs;
    std::vector<Rig> m_rigs;
    std::shared_ptr<std::atomic<bool>> m_alive;  // false once stopped: late answers are dropped
};

}  // namespace api
}  // namespace hashwarp
