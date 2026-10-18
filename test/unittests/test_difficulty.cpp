// Stratum difficulty <-> target conversions (Parallax XHash rules)

#include <libdevcore/CommonData.h>

#include <boost/multiprecision/cpp_int.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace dev;
using BigInt = boost::multiprecision::cpp_int;

namespace
{
const BigInt maxTarget = (BigInt(1) << 256) - 1;

// What pools compute (miningcore-xhash EthereumJob): MaxValue / (difficulty * 2^32)
BigInt poolTarget(double diff)
{
    return maxTarget / BigInt(std::ldexp(diff, 32));
}
}  // namespace

TEST(difficulty, one_is_two_pow_32_hashes)
{
    EXPECT_EQ(BigInt(getTargetFromDiff(1)), maxTarget / (BigInt(1) << 32));
}

TEST(difficulty, matches_pool_target)
{
    for (double diff : {0.0001, 0.5, 1.0, 3.0, 7.25, 1000.0, 4294967296.0, 1e10, 1e15})
        EXPECT_EQ(BigInt(getTargetFromDiff(diff)), poolTarget(diff)) << "difficulty " << diff;
}

TEST(difficulty, invalid_values_give_easiest_target)
{
    for (double diff : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity()})
        EXPECT_EQ(BigInt(getTargetFromDiff(diff)), maxTarget) << "difficulty " << diff;
}

TEST(difficulty, hex_format)
{
    std::string t = getTargetFromDiff(1000);
    EXPECT_EQ(t.size(), 66u);
    EXPECT_EQ(t.substr(0, 2), "0x");
    EXPECT_EQ(t.find_first_of("ABCDEF"), std::string::npos);
    EXPECT_EQ(getTargetFromDiff(1000, HexPrefix::DontAdd), t.substr(2));
}

TEST(difficulty, hashes_to_target_is_inverse)
{
    for (double diff : {0.5, 1.0, 3.0, 1e6, 1e12})
    {
        double hashes = getHashesToTarget(getTargetFromDiff(diff));
        EXPECT_NEAR(hashes / std::ldexp(diff, 32), 1.0, 1e-9) << "difficulty " << diff;
    }
    EXPECT_EQ(getHashesToTarget("0x0"), 0.0);
}

TEST(difficulty, formatted_hashes_do_not_overflow_suffixes)
{
    EXPECT_EQ(getFormattedHashes(115.48e6), "115.48 Mh");
    EXPECT_EQ(getFormattedHashes(2.5e15), "2500.00 Th");
}
