// Multi-rig view: rig addresses and polling other rigs' APIs

#include <libapicore/HttpApi.h>
#include <libapicore/Rigs.h>

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>

using namespace hashwarp::api;
using boost::asio::ip::tcp;

TEST(rigs, parse_addresses)
{
    RigAddress r = parseRig("10.0.0.12");
    EXPECT_EQ(r.name, "10.0.0.12");
    EXPECT_EQ(r.host, "10.0.0.12");
    EXPECT_EQ(r.port, 21550);
    EXPECT_EQ(r.token, "");

    r = parseRig("rig2=http://s3cret@rig2.lan:3333/");
    EXPECT_EQ(r.name, "rig2");
    EXPECT_EQ(r.host, "rig2.lan");
    EXPECT_EQ(r.port, 3333);
    EXPECT_EQ(r.token, "s3cret");

    r = parseRig("[fe80::1]:21551");
    EXPECT_EQ(r.host, "fe80::1");
    EXPECT_EQ(r.port, 21551);

    EXPECT_THROW(parseRig("https://rig:1"), std::invalid_argument);
    EXPECT_THROW(parseRig("rig:0"), std::invalid_argument);
    EXPECT_THROW(parseRig("rig:99999"), std::invalid_argument);
    EXPECT_THROW(parseRig("a=:1"), std::invalid_argument);
    EXPECT_THROW(parseRig("[::1"), std::invalid_argument);
}

namespace
{
// Answers every request with the same response, remembers the last request
class TestServer
{
public:
    explicit TestServer(std::string _response)
      : m_acceptor(m_io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)),
        m_response(std::move(_response))
    {
        m_thread = std::thread([this]() {
            while (!m_stop)
            {
                tcp::socket s(m_io);
                boost::system::error_code ec;
                m_acceptor.accept(s, ec);
                if (ec || m_stop)
                    break;
                std::string req;
                char buf[4096];
                while (req.find("\r\n\r\n") == std::string::npos)
                {
                    size_t n = s.read_some(boost::asio::buffer(buf), ec);
                    if (ec)
                        break;
                    req.append(buf, n);
                }
                {
                    std::lock_guard<std::mutex> l(m_lock);
                    m_request = req;
                }
                boost::asio::write(s, boost::asio::buffer(m_response), ec);
            }
        });
    }
    ~TestServer()
    {
        m_stop = true;
        boost::system::error_code ec;
        tcp::socket wake(m_io);
        wake.connect(m_acceptor.local_endpoint(), ec);
        m_thread.join();
    }
    uint16_t port() const { return m_acceptor.local_endpoint().port(); }
    std::string request()
    {
        std::lock_guard<std::mutex> l(m_lock);
        return m_request;
    }

private:
    boost::asio::io_context m_io;
    tcp::acceptor m_acceptor;
    std::string m_response;
    std::atomic<bool> m_stop = {false};
    std::mutex m_lock;
    std::string m_request;
    std::thread m_thread;
};

// Polls _rigs until each was answered (or failed), returns RigPoller::rigs()
Json::Value poll(const std::vector<RigAddress>& _rigs)
{
    boost::asio::io_context io;
    auto work = boost::asio::make_work_guard(io);
    std::thread t([&]() { io.run(); });
    Json::Value rigs;
    {
        RigPoller poller(io, _rigs, std::chrono::seconds(1));
        poller.start();
        for (int i = 0; i < 100; i++)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            rigs = poller.rigs();
            bool done = true;
            for (const auto& r : rigs)
                done = done && r["error"].asString() != "not polled yet";
            if (done)
                break;
        }
        poller.stop();
    }
    work.reset();
    io.stop();
    t.join();
    return rigs;
}

const char* const c_summary =
    "{\"miner\":{\"version\":\"2.0.0\",\"uptime\":60},\"hashrate\":{\"now\":115e6},"
    "\"shares\":{\"accepted\":3,\"rejected\":1,\"failed\":0},"
    "\"gpus\":[{\"index\":0,\"power\":280.5},{\"index\":1,\"power\":200}]}";
}  // namespace

TEST(rigs, polls_summaries)
{
    TestServer ok(httpResponse(200, "application/json", c_summary));
    TestServer unauthorized(httpResponse(401, "text/plain", "no\n"));
    TestServer notHashwarp(httpResponse(200, "application/json", "{\"hello\":1}"));
    uint16_t closedPort;
    {
        boost::asio::io_context io;
        tcp::acceptor a(io, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
        closedPort = a.local_endpoint().port();
    }

    Json::Value rigs =
        poll({parseRig("a=t0k@127.0.0.1:" + std::to_string(ok.port())),
            parseRig("b=127.0.0.1:" + std::to_string(unauthorized.port())),
            parseRig("c=127.0.0.1:" + std::to_string(notHashwarp.port())),
            parseRig("d=127.0.0.1:" + std::to_string(closedPort))});
    ASSERT_EQ(rigs.size(), 4u);

    EXPECT_TRUE(rigs[0]["online"].asBool()) << rigs[0]["error"].asString();
    EXPECT_EQ(rigs[0]["name"].asString(), "a");
    EXPECT_EQ(rigs[0]["summary"]["hashrate"]["now"].asDouble(), 115e6);
    EXPECT_GE(rigs[0]["age"].asInt(), 0);
    EXPECT_NE(ok.request().find("GET /api/v1/summary HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(ok.request().find("Authorization: Bearer t0k\r\n"), std::string::npos);

    EXPECT_FALSE(rigs[1]["online"].asBool());
    EXPECT_EQ(rigs[1]["error"].asString(), "wrong or missing token");
    EXPECT_EQ(rigs[1]["age"].asInt(), -1);
    EXPECT_FALSE(rigs[2]["online"].asBool());
    EXPECT_EQ(rigs[2]["error"].asString(), "not a hashwarp API");
    EXPECT_FALSE(rigs[3]["online"].asBool());
    EXPECT_FALSE(rigs[3]["error"].asString().empty());

    std::string metrics = metricsFromRigs(rigs);
    EXPECT_NE(metrics.find("hashwarp_rig_up{rig=\"a\"} 1\n"), std::string::npos) << metrics;
    EXPECT_NE(metrics.find("hashwarp_rig_up{rig=\"d\"} 0\n"), std::string::npos);
    EXPECT_NE(metrics.find("hashwarp_rig_hashrate{rig=\"a\"} 115000000\n"), std::string::npos);
    EXPECT_NE(metrics.find("hashwarp_rig_shares_total{rig=\"a\",result=\"rejected\"} 1\n"),
        std::string::npos);
    EXPECT_NE(metrics.find("hashwarp_rig_power_watts{rig=\"a\"} 480.5\n"), std::string::npos);
}
