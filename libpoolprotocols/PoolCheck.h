// Warns about mining on a pool that finds most of the network's blocks.
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dev
{
namespace eth
{
/// The status page that tells the pools' shares of the blocks
extern const char* const c_statusHost;  // status.parallaxprotocol.org
extern const char* const c_statusPath;  // /api/v2/stats

/// A mining pool's share of the network's recent blocks
struct PoolShare
{
    std::string name;                  // e.g. rplant.xyz
    std::vector<std::string> domains;  // its name and website's domains, lower case
    double share = 0;                  // of the blocks, 0 to 1
    unsigned blocks = 0;
};

/// The pools' shares of the last 7 days' blocks, largest first
struct NetworkShares
{
    unsigned blocks = 0;
    std::vector<PoolShare> pools;
};

/// Parses the decentralization figures of the status page's stats. Throws
/// std::runtime_error when they are missing
NetworkShares parseNetworkShares(const std::string& _json);

/// The pool that _host (a stratum or pool host) belongs to, or nullptr
const PoolShare* findPool(const NetworkShares& _shares, const std::string& _host);

/// The warning lines about mining on _pool when it has the majority, none otherwise
std::vector<std::string> majorityWarning(const PoolShare& _pool, const NetworkShares& _all);

/// Checks the active pool against the status page every 10 minutes while mining: logs a
/// warning when it has the majority, then hourly, or right away when the pool changes
class PoolCheck
{
public:
    PoolCheck() = default;
    ~PoolCheck();

    void start();
    void stop();

    static const std::chrono::minutes c_fetchInterval;
    static const std::chrono::minutes c_warnInterval;

private:
    // What the io_service thread's evaluation uses, shared with it: it may run after stop()
    struct State
    {
        std::mutex lock;
        bool alive = true;
        std::string warnedHost;
        std::chrono::steady_clock::time_point warnedAt;
    };

    void run();
    static void evaluate(State& _state, const NetworkShares& _shares);

    std::thread m_thread;
    std::mutex x_stop;
    std::condition_variable m_stopped;
    bool m_stop = false;
    std::shared_ptr<State> m_state = std::make_shared<State>();
};

}  // namespace eth
}  // namespace dev
