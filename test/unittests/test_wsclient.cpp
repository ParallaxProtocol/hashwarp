// Work packages of the WebSocket solo client

#include <libpoolprotocols/getwork/EthWsClient.h>

#include <json/json.h>
#include <gtest/gtest.h>

#include <sstream>

namespace
{
Json::Value parse(const std::string& _json)
{
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in(_json);
    Json::parseFromStream(builder, in, &v, &errors);
    return v;
}

const char* const c_header = "0x7d42fe8f6f1e3c0a5d2b9e8f1a3c5e7d9b1f3a5c7e9d1b3f5a7c9e1d3b5f7a9c";
const char* const c_seed = "0x5d4c3b2a19087f6e5d4c3b2a19087f6e5d4c3b2a19087f6e5d4c3b2a19087f6e";
const char* const c_boundary = "0x0000000112e0be826d694b2e62d01511f12a6061fbaec8bc02357593e70e52ba";
}  // namespace

TEST(wsclient, parses_work)
{
    WorkPackage wp;
    std::string work = std::string("[\"") + c_header + "\",\"" + c_seed + "\",\"" + c_boundary +
                       "\",\"0xec54\"]";
    ASSERT_TRUE(EthWsClient::parseWork(parse(work), wp));
    EXPECT_EQ("0x" + wp.header.hex(), c_header);
    EXPECT_EQ("0x" + wp.boundary.hex(), c_boundary);
    EXPECT_EQ(wp.block, 60500);

    // Without the block number (other nodes)
    work = std::string("[\"") + c_header + "\",\"" + c_seed + "\",\"" + c_boundary + "\"]";
    ASSERT_TRUE(EthWsClient::parseWork(parse(work), wp));
    EXPECT_EQ(wp.block, -1);

    EXPECT_FALSE(EthWsClient::parseWork(parse("[\"0x1\",\"0x2\"]"), wp));
    EXPECT_FALSE(EthWsClient::parseWork(parse("{\"header\":1}"), wp));
    EXPECT_FALSE(EthWsClient::parseWork(parse("[1,2,3]"), wp));
}
