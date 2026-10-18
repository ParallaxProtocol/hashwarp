// Nonce space splitting among miners

#include <libparallaxcore/Farm.h>

#include <gtest/gtest.h>

using dev::eth::Farm;

TEST(nonce_segments, whole_space_without_extranonce)
{
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 1, 0), 64u);
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 2, 0), 63u);
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 3, 0), 62u);
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 8, 0), 61u);
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 9, 0), 60u);
}

TEST(nonce_segments, extranonce_reduces_space)
{
    // Extranonce size is expressed in hex chars (4 bits each)
    EXPECT_EQ(Farm::nonceSegmentWidth(4, 1, 0), 48u);
    EXPECT_EQ(Farm::nonceSegmentWidth(6, 6, 0), 37u);
    EXPECT_EQ(Farm::nonceSegmentWidth(8, 1, 0), 32u);
}

TEST(nonce_segments, no_space_left)
{
    EXPECT_EQ(Farm::nonceSegmentWidth(16, 1, 0), 0u);
    EXPECT_EQ(Farm::nonceSegmentWidth(20, 1, 0), 0u);
    EXPECT_EQ(Farm::nonceSegmentWidth(15, 32, 0), 0u);
}

TEST(nonce_segments, user_width_only_narrows)
{
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 4, 40), 40u);
    EXPECT_EQ(Farm::nonceSegmentWidth(0, 4, 63), 62u);
    // Ignored when the pool sets an extranonce
    EXPECT_EQ(Farm::nonceSegmentWidth(4, 1, 40), 48u);
}
