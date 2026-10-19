// The pools' shares of the blocks, from the status page

#include <libpoolprotocols/PoolCheck.h>

#include <gtest/gtest.h>

using namespace dev::eth;

namespace
{
// Trimmed from status.parallaxprotocol.org/api/v2/stats
const char* const c_stats = R"({"network":"mainnet","decentralization":{"blocks":968,
  "grade":"critical","topMiners":[
  {"name":"coolpool.top","website":"https://coolpool.top/pool/prlx","address":"0x6c","blocks":130,"share":0.1343},
  {"name":"rplant.xyz","website":"https://pool.rplant.xyz/","address":"0x38","blocks":830,"share":0.8574},
  {"name":"gtpool.io\u001b[31m","website":null,"address":"0x03","blocks":8,"share":0.0083},
  {"address":"0x99","blocks":1,"share":0.001}]}})";
}  // namespace

TEST(poolcheck, parses_shares)
{
    NetworkShares s = parseNetworkShares(c_stats);
    EXPECT_EQ(s.blocks, 968u);
    ASSERT_EQ(s.pools.size(), 3u);  // the address without a pool name is left out
    EXPECT_EQ(s.pools[0].name, "rplant.xyz");  // largest first
    EXPECT_EQ(s.pools[0].blocks, 830u);
    EXPECT_EQ(s.pools[2].name, "gtpool.io31m");  // no terminal escapes
    EXPECT_THROW(parseNetworkShares("{}"), std::runtime_error);
    EXPECT_THROW(parseNetworkShares("<html>"), std::runtime_error);
}

TEST(poolcheck, finds_pools_by_host)
{
    NetworkShares s = parseNetworkShares(c_stats);
    ASSERT_NE(findPool(s, "stratum-eu.rplant.xyz"), nullptr);
    EXPECT_EQ(findPool(s, "stratum-eu.rplant.xyz")->name, "rplant.xyz");
    EXPECT_EQ(findPool(s, "RPLANT.XYZ")->name, "rplant.xyz");
    EXPECT_EQ(findPool(s, "prlx.coolpool.top")->name, "coolpool.top");
    EXPECT_EQ(findPool(s, "notrplant.xyz"), nullptr);
    EXPECT_EQ(findPool(s, "pool.ion.cool"), nullptr);
    EXPECT_EQ(findPool(s, "127.0.0.1"), nullptr);
}

TEST(poolcheck, warns_about_a_majority)
{
    NetworkShares s = parseNetworkShares(c_stats);
    auto lines = majorityWarning(*findPool(s, "stratum-na.rplant.xyz"), s);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].compare(0, 54, "rplant.xyz found 86% of the network's blocks in the la"), 0)
        << lines[0];
    EXPECT_NE(lines[1].find("coolpool.top 13% · gtpool.io31m 1%"), std::string::npos) << lines[1];
    EXPECT_TRUE(majorityWarning(*findPool(s, "prlx.coolpool.top"), s).empty());
}
