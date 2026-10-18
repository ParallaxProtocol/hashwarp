// HTTP API helpers: request parsing, secrets, Prometheus metrics

#include <libapicore/HttpApi.h>

#include <gtest/gtest.h>

using namespace hashwarp::api;

TEST(httpapi, detects_http)
{
    EXPECT_TRUE(looksLikeHttp("GET / HTTP/1.1\r\n"));
    EXPECT_TRUE(looksLikeHttp("POS"));  // may still become a POST
    EXPECT_FALSE(looksLikeHttp("{\"id\":1}\n"));
}

TEST(httpapi, parses_requests)
{
    HttpRequest r;
    size_t consumed = 0;
    std::string get = "GET /metrics?x=1 HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer t0k\r\n\r\n";
    ASSERT_EQ(parseHttpRequest(get, r, consumed), ParseResult::Complete);
    EXPECT_EQ(r.method, "GET");
    EXPECT_EQ(r.path, "/metrics");
    EXPECT_EQ(r.query, "x=1");
    EXPECT_EQ(r.headers["authorization"], "Bearer t0k");
    EXPECT_EQ(consumed, get.size());

    // Headers and body arriving in pieces
    std::string post =
        "POST /api/v1/control HTTP/1.1\r\nContent-Length: 17\r\n\r\n{\"action\":\"stop\"}";
    EXPECT_EQ(parseHttpRequest(post.substr(0, 20), r, consumed), ParseResult::Incomplete);
    EXPECT_EQ(parseHttpRequest(post.substr(0, post.size() - 3), r, consumed), ParseResult::Incomplete);
    ASSERT_EQ(parseHttpRequest(post, r, consumed), ParseResult::Complete);
    EXPECT_EQ(r.body, "{\"action\":\"stop\"}");

    EXPECT_EQ(parseHttpRequest("GET nope HTTP/1.1\r\n\r\n", r, consumed), ParseResult::Invalid);
    EXPECT_EQ(parseHttpRequest("POST / HTTP/1.1\r\nContent-Length: 999999\r\n\r\n", r, consumed),
        ParseResult::Invalid);
    EXPECT_EQ(parseHttpRequest(std::string(c_maxRequestSize + 1, 'x'), r, consumed),
        ParseResult::Invalid);
}

TEST(httpapi, responses)
{
    std::string resp = httpResponse(404, "text/plain", "Not found\n");
    EXPECT_EQ(resp.compare(0, 22, "HTTP/1.1 404 Not Found"), 0);
    EXPECT_NE(resp.find("Content-Length: 10\r\n"), std::string::npos);
    EXPECT_NE(resp.find("Connection: close\r\n\r\nNot found\n"), std::string::npos);
}

TEST(httpapi, secrets)
{
    EXPECT_TRUE(secretEquals("s3cret", "s3cret"));
    EXPECT_FALSE(secretEquals("s3cret", "s3creT"));
    EXPECT_FALSE(secretEquals("s3cret", "s3cre"));
    EXPECT_FALSE(secretEquals("", "x"));
}

TEST(httpapi, metrics)
{
    Json::Value s;
    s["miner"]["version"] = "2.0.0";
    s["miner"]["uptime"] = 120;
    s["hashrate"]["now"] = 115480000.0;
    s["hashrate"]["60s"] = 115400000.0;
    s["hashrate"]["15m"] = 115200000.0;
    s["hashrate"]["effective"] = 112300000.0;
    s["shares"]["accepted"] = 1234567;
    s["shares"]["rejected"] = 1;
    s["shares"]["failed"] = 0;
    s["pool"]["host"] = "pool.example.com:3333";
    s["pool"]["connected"] = true;
    s["pool"]["difficulty"] = 4294967296.0;
    s["pool"]["epoch"] = 83;
    s["pool"]["block"] = 60268;
    s["pool"]["switches"] = 0;
    Json::Value g;
    g["index"] = 0;
    g["name"] = "RTX \"4090\"";
    g["hashrate"]["now"] = 115480000.0;
    g["shares"]["accepted"] = 1234567;
    g["paused"] = false;
    g["temperature"]["core"] = 65;
    g["temperature"]["memory"] = 0;
    g["fan"] = 70;
    g["power"] = 280.5;
    g["clocks"]["core"] = 2790;
    g["clocks"]["memory"] = 10251;
    s["gpus"].append(g);

    std::string m = metricsFromSummary(s);
    auto has = [&](const std::string& _line) {
        return m.find("\n" + _line + "\n") != std::string::npos;
    };
    EXPECT_TRUE(has("hashwarp_info{version=\"2.0.0\"} 1")) << m;
    EXPECT_TRUE(has("hashwarp_hashrate{window=\"now\"} 115480000"));
    EXPECT_TRUE(has("hashwarp_hashrate{gpu=\"0\",name=\"RTX \\\"4090\\\"\",window=\"now\"} 115480000"));
    EXPECT_TRUE(has("hashwarp_shares_total{result=\"accepted\"} 1234567")) << "no rounding";
    EXPECT_TRUE(has("hashwarp_gpu_temperature_celsius{gpu=\"0\",name=\"RTX \\\"4090\\\"\",sensor=\"core\"} 65"));
    EXPECT_EQ(m.find("sensor=\"memory\""), std::string::npos) << "unreported sensors are omitted";
    EXPECT_TRUE(has("hashwarp_gpu_power_watts{gpu=\"0\",name=\"RTX \\\"4090\\\"\"} 280.5"));
    EXPECT_TRUE(has("hashwarp_pool_connected{host=\"pool.example.com:3333\"} 1"));
    EXPECT_TRUE(has("hashwarp_block_height 60268"));
    EXPECT_NE(m.find("# TYPE hashwarp_shares_total counter"), std::string::npos);
}
