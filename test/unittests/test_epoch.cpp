// XHash epoch and seed rules (720 block epochs, PARALLAX magic in the seed chain)

#include <xhash/sha3.hpp>
#include <xhash/xhash.hpp>

#include <gtest/gtest.h>

#include <cstring>

TEST(epoch, length_is_720_blocks)
{
    EXPECT_EQ(xhash::epoch_length, 720);
    EXPECT_EQ(xhash::get_epoch_number(0), 0);
    EXPECT_EQ(xhash::get_epoch_number(719), 0);
    EXPECT_EQ(xhash::get_epoch_number(720), 1);
    EXPECT_EQ(xhash::get_epoch_number(60268), 83);
}

TEST(epoch, seed_chain_uses_parallax_magic)
{
    // seed(1) = sha3_256("PARALLAX\x01" || seed(0)) with seed(0) all zeroes
    static const uint8_t magic[] = {'P', 'A', 'R', 'A', 'L', 'L', 'A', 'X', 0x01};
    uint8_t buf[sizeof(magic) + 32] = {};
    std::memcpy(buf, magic, sizeof(magic));

    auto expected = xhash::sha3_256(buf, sizeof(buf));
    auto seed = xhash::calculate_epoch_seed(1);
    EXPECT_EQ(0, std::memcmp(seed.bytes, expected.bytes, sizeof(seed.bytes)));
}

TEST(epoch, epoch_is_recovered_from_seed)
{
    for (int epoch : {0, 1, 2, 83, 84, 82, 1000, 83})
        EXPECT_EQ(xhash::find_epoch_number(xhash::calculate_epoch_seed(epoch)), epoch)
            << "epoch " << epoch;
}

TEST(epoch, unknown_seed)
{
    xhash::hash256 seed = {};
    seed.word32s[0] = 0xdeadbeef;
    EXPECT_EQ(xhash::find_epoch_number(seed), -1);
}
