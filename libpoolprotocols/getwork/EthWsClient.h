// Solo mining over a node's WebSocket: the node pushes new work (eth_subscribe newWork).
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <json/json.h>

#include <deque>
#include <memory>
#include <string>
#include <type_traits>

#include "../PoolClient.h"

using namespace dev;
using namespace eth;

/// ws:// and wss:// URLs of a Parallax node. The node pushes each new work package
/// (eth_subscribe "newWork"); a node without the subscription is polled with
/// eth_getWork on the same connection. Solutions go back with eth_submitWork
class EthWsClient : public PoolClient
{
public:
    EthWsClient(int _workTimeout, unsigned _pollPeriodMs);
    ~EthWsClient();

    void connect() override;
    void disconnect() override;

    void submitHashrate(uint64_t const& _rate, string const& _id) override;
    void submitSolution(const Solution& _solution) override;

    /// The work package of an eth_getWork result or newWork notification: [header, seed,
    /// boundary, block number]. Returns false if it isn't one
    static bool parseWork(const Json::Value& _result, WorkPackage& _wp);

private:
    using TcpStream = boost::beast::tcp_stream;
    using Ws = boost::beast::websocket::stream<TcpStream>;
    using Wss = boost::beast::websocket::stream<boost::beast::ssl_stream<TcpStream>>;

    // Runs _f on whichever stream is in use
    template <class F>
    void withStream(F _f)
    {
        if (m_wss)
            _f(*m_wss);
        else if (m_ws)
            _f(*m_ws);
    }

    // A handler that does nothing once the client is gone
    template <class H>
    struct Guarded
    {
        std::weak_ptr<bool> alive;
        H handler;

        // Only invocable as the handler is: Asio picks overloads by what a handler accepts
        template <class... A, class = std::enable_if_t<std::is_invocable_v<H&, A&&...>>>
        void operator()(A&&... _args)
        {
            if (alive.lock())
                handler(std::forward<A>(_args)...);
        }
    };

    // The handlers run on the io_service thread
    template <class H>
    auto guard(H _h)
    {
        return boost::asio::bind_executor(m_strand, Guarded<H>{m_alive, std::move(_h)});
    }

    void connected();
    void read();
    void received(const std::string& _message);
    void newWork(const WorkPackage& _wp);
    void send(Json::Value _request);
    void writeNext();
    void poll();
    void failed(const std::string& _what, const boost::system::error_code& _ec);
    void closed();

    boost::asio::io_service::strand m_strand;
    boost::asio::ip::tcp::resolver m_resolver;
    boost::asio::ssl::context m_tls;
    std::unique_ptr<Ws> m_ws;
    std::unique_ptr<Wss> m_wss;
    boost::beast::flat_buffer m_buffer;
    std::deque<std::string> m_writes;  // the front one is being written
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

    boost::asio::deadline_timer m_pollTimer;
    unsigned m_pollPeriodMs;
    bool m_polling = false;  // the node has no newWork subscription
    std::string m_subscription;
    int m_workTimeout;

    WorkPackage m_current;
    std::chrono::steady_clock::time_point m_currentAt;
    std::chrono::steady_clock::time_point m_submittedAt;
    unsigned m_maxSubmitId = 0;
    bool m_closing = false;
};
