// Warns about mining on a pool that finds most of the network's blocks.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#define LOG_SOURCE "pool"

#include "PoolCheck.h"
#include "PoolManager.h"
#include "Tls.h"

#include <libdevcore/Log.h>

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace dev
{
namespace eth
{
const char* const c_statusHost = "status.parallaxprotocol.org";
const char* const c_statusPath = "/api/v2/stats";
const std::chrono::minutes PoolCheck::c_fetchInterval(10);
const std::chrono::minutes PoolCheck::c_warnInterval(60);

namespace
{
// The first fetch waits for the pool connection
const std::chrono::seconds c_firstFetch(15);

std::string lower(std::string _s)
{
    std::transform(_s.begin(), _s.end(), _s.begin(), [](unsigned char c) { return std::tolower(c); });
    return _s;
}

bool isDomain(const std::string& _s)
{
    return _s.find('.') != std::string::npos && _s.front() != '.' && _s.back() != '.' &&
           _s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") == std::string::npos;
}

// The host of an http(s) URL, lower case
std::string urlHost(const std::string& _url)
{
    std::string s = lower(_url);
    size_t scheme = s.find("://");
    if (scheme == std::string::npos)
        return "";
    s = s.substr(scheme + 3);
    s = s.substr(0, s.find_first_of("/:?#"));
    return s;
}

// Text from the status page as plain text: no terminal escapes nor markup
std::string printable(const std::string& _s)
{
    std::string ret;
    for (char c : _s)
        if (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ' ')
            ret += c;
    return ret.substr(0, 40);
}

std::string percent(double _share)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", _share * 100);
    return buf;
}
}  // namespace

NetworkShares parseNetworkShares(const std::string& _json)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(_json);
    if (!Json::parseFromStream(builder, in, &root, &errors) || !root.isObject())
        throw std::runtime_error("the status page sent invalid JSON");
    const Json::Value& d = root["decentralization"];
    if (!d.isObject() || !d["topMiners"].isArray())
        throw std::runtime_error("the status page has no decentralization figures");

    NetworkShares ret;
    ret.blocks = d.get("blocks", 0).asUInt();
    for (const auto& m : d["topMiners"])
    {
        if (!m.isObject() || !m["name"].isString() || !m["share"].isNumeric())
            continue;  // an address without a known pool: nothing to match
        PoolShare p;
        p.name = printable(m["name"].asString());
        p.share = std::min(1.0, std::max(0.0, m["share"].asDouble()));
        p.blocks = m.get("blocks", 0).asUInt();
        std::string name = lower(m["name"].asString());
        if (isDomain(name))
            p.domains.push_back(name);
        if (m["website"].isString())
        {
            // pool.example.com: also example.com
            std::string host = urlHost(m["website"].asString());
            if (isDomain(host))
            {
                p.domains.push_back(host);
                for (const char* prefix : {"www.", "pool.", "pools.", "mining."})
                    if (host.compare(0, strlen(prefix), prefix) == 0 &&
                        isDomain(host.substr(strlen(prefix))))
                        p.domains.push_back(host.substr(strlen(prefix)));
            }
        }
        ret.pools.push_back(p);
    }
    std::sort(ret.pools.begin(), ret.pools.end(),
        [](const PoolShare& _a, const PoolShare& _b) { return _a.share > _b.share; });
    return ret;
}

const PoolShare* findPool(const NetworkShares& _shares, const std::string& _host)
{
    std::string host = lower(_host);
    for (const auto& p : _shares.pools)
        for (const auto& d : p.domains)
            if (host == d ||
                (host.size() > d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0 &&
                    host[host.size() - d.size() - 1] == '.'))
                return &p;
    return nullptr;
}

std::vector<std::string> majorityWarning(const PoolShare& _pool, const NetworkShares& _all)
{
    if (_pool.share <= 0.5)
        return {};
    std::vector<std::string> lines;
    lines.push_back(_pool.name + " found " + percent(_pool.share) +
                    " of the network's blocks in the last 7 days. A pool with the majority can "
                    "reverse payments and block transactions, which puts the coin you mine at "
                    "risk");
    std::string others;
    for (const auto& p : _all.pools)
        if (&p != &_pool && p.share < 0.5 && !p.name.empty())
            others += (others.empty() ? "" : " · ") + p.name + " " + percent(p.share);
    lines.push_back("Please mine on a smaller pool" + (others.empty() ? "" : " (" + others + ")") +
                    " or solo (--solo). https://" + c_statusHost +
                    " has the figures; --no-pool-check hides this");
    return lines;
}

PoolCheck::~PoolCheck()
{
    stop();
}

void PoolCheck::start()
{
    m_thread = std::thread([this]() {
        setThreadName("poolcheck");
        run();
    });
}

void PoolCheck::stop()
{
    {
        std::lock_guard<std::mutex> l(m_state->lock);
        m_state->alive = false;
    }
    {
        std::lock_guard<std::mutex> l(x_stop);
        m_stop = true;
    }
    m_stopped.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

void PoolCheck::run()
{
    std::chrono::steady_clock::duration wait = c_firstFetch;
    while (true)
    {
        {
            std::unique_lock<std::mutex> l(x_stop);
            if (m_stopped.wait_for(l, wait, [this]() { return m_stop; }))
                return;
        }
        wait = c_fetchInterval;
        try
        {
            NetworkShares shares = parseNetworkShares(
                httpsGet(c_statusHost, c_statusPath, std::chrono::seconds(15)));
            auto state = m_state;
            g_io_service.post([state, shares]() { evaluate(*state, shares); });
        }
        catch (const std::exception& _ex)
        {
            // Never in the way of mining: the next fetch may work
            cdebug << "Can't check the pool's share of the blocks: " << _ex.what();
        }
    }
}

void PoolCheck::evaluate(State& _state, const NetworkShares& _shares)
{
    std::lock_guard<std::mutex> l(_state.lock);
    if (!_state.alive)
        return;
    PoolManager& pm = PoolManager::p();
    auto conn = pm.getActiveConnection();
    const PoolShare* pool = nullptr;
    if (conn && pm.isConnected() && conn->Family() != ProtocolFamily::GETWORK)
        pool = findPool(_shares, conn->Host());
    std::vector<std::string> lines;
    if (pool)
        lines = majorityWarning(*pool, _shares);
    if (lines.empty())
    {
        // Warn at once if the pool gets the majority again
        _state.warnedHost.clear();
        pm.setPoolWarning("");
        return;
    }

    pm.setPoolWarning(lines[0]);
    auto now = std::chrono::steady_clock::now();
    if (conn->Host() == _state.warnedHost && now - _state.warnedAt < c_warnInterval)
        return;
    _state.warnedHost = conn->Host();
    _state.warnedAt = now;
    cwarn.with("event", "pool_majority").with("pool", pool->name).with("share", pool->share)
        << EthYellowBold << lines[0] << EthReset;
    for (size_t i = 1; i < lines.size(); i++)
        cwarn << lines[i];
}

}  // namespace eth
}  // namespace dev
