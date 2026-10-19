// Solo mining over a node's WebSocket: the node pushes new work (eth_subscribe newWork).
// Licensed under GNU General Public License, Version 3. See the LICENSE file.

#define LOG_SOURCE "node"

#include "EthWsClient.h"
#include "../Tls.h"

#include <hashwarp/buildinfo.h>
#include <libdevcore/Log.h>

#include <sstream>

namespace websocket = boost::beast::websocket;
using boost::asio::ip::tcp;

namespace
{
enum RequestId : unsigned
{
    c_subscribe = 1,
    c_getWork = 2,
    c_hashrate = 9,
    c_submit = 40  // + the GPU index
};

const std::chrono::seconds c_connectTimeout(10);
const unsigned c_timeoutCheckMs = 10000;

std::string errorText(const Json::Value& _error)
{
    if (_error.isObject() && _error["message"].isString())
        return _error["message"].asString();
    if (_error.isString())
        return _error.asString();
    return "unknown error";
}
}  // namespace

EthWsClient::EthWsClient(int _workTimeout, unsigned _pollPeriodMs)
  : PoolClient(),
    m_strand(g_io_service),
    m_resolver(g_io_service),
    m_tls(boost::asio::ssl::context::tls_client),
    m_pollTimer(g_io_service),
    m_pollPeriodMs(_pollPeriodMs),
    m_workTimeout(_workTimeout)
{
    loadCaCertificates(m_tls);
}

EthWsClient::~EthWsClient()
{
    m_alive.reset();
}

bool EthWsClient::parseWork(const Json::Value& _result, WorkPackage& _wp)
{
    if (!_result.isArray() || _result.size() < 3)
        return false;
    for (Json::ArrayIndex i = 0; i < _result.size(); i++)
        if (!_result[i].isString())
            return false;
    try
    {
        _wp.header = h256(_result[0].asString());
        _wp.seed = h256(_result[1].asString());
        _wp.boundary = h256(_result[2].asString());
        _wp.job = _wp.header.hex();
        // Parallax nodes append the block number being mined
        _wp.block = _result.size() > 3 ? (int)std::stoul(_result[3].asString(), nullptr, 16) : -1;
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

void EthWsClient::connect()
{
    m_closing = false;
    m_polling = false;
    m_subscription.clear();
    m_writes.clear();
    m_buffer.clear();
    m_current = WorkPackage();
    m_ws.reset();
    m_wss.reset();
    if (m_conn->SecLevel() != SecureLevel::NONE)
    {
        m_wss.reset(new Wss(g_io_service, m_tls));
        // The name for servers with several sites (SNI), and the one the certificate must have
        SSL_set_tlsext_host_name(m_wss->next_layer().native_handle(), m_conn->Host().c_str());
        if (getenv("SSL_NOVERIFY"))
            m_wss->next_layer().set_verify_mode(boost::asio::ssl::verify_none);
        else
        {
            m_wss->next_layer().set_verify_mode(boost::asio::ssl::verify_peer);
            m_wss->next_layer().set_verify_callback(
                boost::asio::ssl::host_name_verification(m_conn->Host()));
        }
    }
    else
        m_ws.reset(new Ws(g_io_service));

    m_resolver.async_resolve(m_conn->Host(), std::to_string(m_conn->Port()),
        guard([this](const boost::system::error_code& _ec, tcp::resolver::results_type _results) {
            if (_ec)
                return failed("Can't resolve", _ec);
            withStream([&](auto& _ws) {
                auto& socket = boost::beast::get_lowest_layer(_ws);
                socket.expires_after(c_connectTimeout);
                socket.async_connect(_results,
                    guard([this](const boost::system::error_code& _ec, const tcp::endpoint& _ep) {
                        if (_ec)
                            return failed("Can't connect to", _ec);
                        m_endpoint = _ep;
                        if (!m_wss)
                            return connected();
                        boost::beast::get_lowest_layer(*m_wss).expires_after(c_connectTimeout);
                        m_wss->next_layer().async_handshake(boost::asio::ssl::stream_base::client,
                            guard([this](const boost::system::error_code& _ec) {
                                if (_ec)
                                    return failed("TLS handshake with", _ec);
                                connected();
                            }));
                    }));
            });
        }));
}

// TCP (and TLS) connected: the WebSocket handshake, then the newWork subscription
void EthWsClient::connected()
{
    withStream([&](auto& _ws) {
        boost::beast::get_lowest_layer(_ws).expires_never();
        websocket::stream_base::timeout timeouts;
        timeouts.handshake_timeout = c_connectTimeout;
        timeouts.idle_timeout = std::chrono::seconds(60);  // pings the node when idle
        timeouts.keep_alive_pings = true;
        _ws.set_option(timeouts);
        _ws.read_message_max(1 << 20);
        _ws.text(true);
        std::string version = hashwarp_get_buildinfo()->project_version;
        std::string agent = "Hashwarp/" + version.substr(0, version.find('+'));
        _ws.set_option(websocket::stream_base::decorator([agent](websocket::request_type& _req) {
            _req.set(boost::beast::http::field::user_agent, agent);
        }));
        std::string host = m_conn->Host() + ":" + std::to_string(m_conn->Port());
        std::string path = m_conn->Path().empty() ? "/" : m_conn->Path();
        _ws.async_handshake(host, path, guard([this](const boost::system::error_code& _ec) {
            if (_ec)
                return failed("WebSocket handshake with", _ec);
            m_connected.store(true, std::memory_order_relaxed);
            m_session.reset(new Session);
            m_session->subscribed.store(true, std::memory_order_relaxed);
            m_session->authorized.store(true, std::memory_order_relaxed);
            m_currentAt = std::chrono::steady_clock::now();
            if (m_onConnected)
                m_onConnected();

            Json::Value req;
            req["id"] = c_subscribe;
            req["method"] = "eth_subscribe";
            req["params"].append("newWork");
            send(req);
            read();
            poll();
        }));
    });
}

void EthWsClient::read()
{
    withStream([&](auto& _ws) {
        _ws.async_read(m_buffer, guard([this](const boost::system::error_code& _ec, size_t) {
            if (_ec)
                return failed("Read from", _ec);
            std::string message = boost::beast::buffers_to_string(m_buffer.data());
            m_buffer.consume(m_buffer.size());
            received(message);
            if (!m_closing)
                read();
        }));
    });
}

void EthWsClient::received(const std::string& _message)
{
    Json::Value msg;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(_message);
    if (!Json::parseFromStream(builder, in, &msg, &errors) || !msg.isObject())
    {
        cwarn << "Invalid JSON from the node: " << errors;
        return;
    }
    if (g_logOptions & LOG_JSON)
        cdebug << " << " << _message;

    // A new work package pushed by the node
    if (msg["method"].asString() == "eth_subscription")
    {
        const Json::Value& params = msg["params"];
        WorkPackage wp;
        if (params["subscription"].asString() == m_subscription && parseWork(params["result"], wp))
            newWork(wp);
        return;
    }

    unsigned id = msg["id"].isUInt() ? msg["id"].asUInt() : 0;
    bool ok = !msg.isMember("error") || msg["error"].isNull();
    if (id == c_subscribe)
    {
        if (ok && msg["result"].isString())
        {
            m_subscription = msg["result"].asString();
            cnote << "The node pushes new work";
            return;
        }
        // A node without the subscription: ask it on this connection
        m_polling = true;
        cnote << "The node can't push work (" << errorText(msg["error"]) << "), asking it every "
              << m_pollPeriodMs << " ms. An updated node pushes it";
        Json::Value req;
        req["id"] = c_getWork;
        req["method"] = "eth_getWork";
        req["params"] = Json::Value(Json::arrayValue);
        send(req);
        m_pollTimer.cancel();
        poll();
    }
    else if (id == c_getWork)
    {
        WorkPackage wp;
        if (!ok)
            cwarn << "Error \"" << errorText(msg["error"]) << "\" from the node";
        else if (parseWork(msg["result"], wp))
            newWork(wp);
        else
            cwarn << "Incomplete eth_getWork response from the node";
    }
    else if (id >= c_submit && id <= m_maxSubmitId)
    {
        if (ok && msg["result"].isBool())
            ok = msg["result"].asBool();
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_submittedAt);
        unsigned gpu = id - c_submit;
        if (ok && m_onSolutionAccepted)
            m_onSolutionAccepted(delay, gpu, false);
        else if (!ok && m_onSolutionRejected)
            m_onSolutionRejected(
                delay, gpu, msg["error"].isNull() ? "the node refused it" : errorText(msg["error"]));
    }
}

void EthWsClient::newWork(const WorkPackage& _wp)
{
    m_currentAt = std::chrono::steady_clock::now();
    if (_wp.header == m_current.header)
        return;
    m_current = _wp;
    if (m_onWorkReceived)
        m_onWorkReceived(m_current);
}

void EthWsClient::send(Json::Value _request)
{
    _request["jsonrpc"] = "2.0";
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string line = Json::writeString(writer, _request);
    if (g_logOptions & LOG_JSON)
        cdebug << " >> " << line;
    m_writes.push_back(line);
    if (m_writes.size() == 1)
        writeNext();
}

void EthWsClient::writeNext()
{
    withStream([&](auto& _ws) {
        _ws.async_write(boost::asio::buffer(m_writes.front()),
            guard([this](const boost::system::error_code& _ec, size_t) {
                if (_ec)
                    return failed("Write to", _ec);
                m_writes.pop_front();
                if (!m_writes.empty())
                    writeNext();
            }));
    });
}

// Asks for work when the node can't push it, and checks that work keeps coming
void EthWsClient::poll()
{
    m_pollTimer.expires_from_now(
        boost::posix_time::milliseconds(m_polling ? m_pollPeriodMs : c_timeoutCheckMs));
    m_pollTimer.async_wait(guard([this](const boost::system::error_code& _ec) {
        if (_ec || m_closing)
            return;
        auto idle = std::chrono::steady_clock::now() - m_currentAt;
        if (idle > std::chrono::seconds(m_workTimeout))
        {
            cwarn << "No new job from the node in " << m_workTimeout << " s";
            disconnect();
            return;
        }
        if (m_polling)
        {
            Json::Value req;
            req["id"] = c_getWork;
            req["method"] = "eth_getWork";
            req["params"] = Json::Value(Json::arrayValue);
            send(req);
        }
        poll();
    }));
}

void EthWsClient::submitHashrate(uint64_t const& _rate, string const& _id)
{
    Json::Value req;
    req["id"] = c_hashrate;
    req["method"] = "eth_submitHashrate";
    req["params"].append(toHex(_rate, HexPrefix::Add));
    req["params"].append(_id);
    boost::asio::post(guard([this, req]() {
        if (m_connected.load(std::memory_order_relaxed))
            send(req);
    }));
}

void EthWsClient::submitSolution(const Solution& _solution)
{
    Json::Value req;
    req["id"] = c_submit + _solution.midx;
    req["method"] = "eth_submitWork";
    req["params"].append("0x" + toHex(_solution.nonce));
    req["params"].append("0x" + _solution.work.header.hex());
    req["params"].append("0x" + _solution.mixHash.hex());
    // From a GPU's thread: the connection is the io_service thread's
    boost::asio::post(guard([this, req, midx = _solution.midx]() {
        if (!m_connected.load(std::memory_order_relaxed))
            return;
        m_maxSubmitId = std::max(m_maxSubmitId, (unsigned)c_submit + midx);
        m_submittedAt = std::chrono::steady_clock::now();
        send(req);
    }));
}

void EthWsClient::failed(const std::string& _what, const boost::system::error_code& _ec)
{
    if (m_closing)
        return;
    if (_ec == websocket::error::closed)
        cnote << m_conn->Host() << " closed the connection";
    else
        cwarn << _what << " " << m_conn->Host() << ":" << m_conn->Port() << ": " << _ec.message();
    disconnect();
}

void EthWsClient::disconnect()
{
    if (m_closing)
        return;
    m_closing = true;
    m_pollTimer.cancel();
    m_resolver.cancel();
    withStream([&](auto& _ws) {
        boost::system::error_code ec;
        boost::beast::get_lowest_layer(_ws).socket().close(ec);
    });
    closed();
}

void EthWsClient::closed()
{
    m_connected.store(false, std::memory_order_relaxed);
    if (m_session)
        m_conn->addDuration(m_session->duration());
    m_session = nullptr;
    if (m_onDisconnected)
        m_onDisconnected();
}
