// Console summary line and statistics table

#include <hashwarp/Console.h>

#include <gtest/gtest.h>

using namespace hashwarp;
using namespace dev::eth;

namespace
{
std::string strip(const std::string& _s)
{
    std::string ret;
    bool skip = false;
    for (char c : _s)
    {
        if (c == '\x1b')
            skip = true;
        else if (skip && c == 'm')
            skip = false;
        else if (!skip)
            ret += c;
    }
    return ret;
}

TelemetryType twoGpus()
{
    TelemetryType t;
    t.hwmon = true;
    TelemetryAccountType a, b;
    a.prefix = b.prefix = "cu";
    a.hashrate = 115.48e6f;
    a.hashrate60 = 115.4e6f;
    a.hashrate900 = 115.2e6f;
    a.sensors.tempC = 65;
    a.sensors.fanP = 70;
    a.sensors.powerW = 280;
    a.solutions.accepted = 42;
    b.prefix = "cu";
    b.paused = true;
    t.miners = {a, b};
    t.farm.hashrate = 115.48e6f;
    t.farm.hashrate60 = 115.4e6f;
    t.farm.hashrate900 = 115.2e6f;
    t.farm.solutions.accepted = 42;
    t.farm.solutions.rejected = 1;
    t.effectiveHashrate = 112.3e6;
    return t;
}
}  // namespace

TEST(console, summary_line)
{
    PoolStatus pool;
    pool.connected = true;
    std::string line = strip(summaryLine(twoGpus(), pool));
    EXPECT_NE(line.find("115.48 Mh/s (60s 115.40, 15m 115.20)"), std::string::npos) << line;
    EXPECT_NE(line.find("eff 112.30"), std::string::npos) << line;
    EXPECT_NE(line.find("A42 R1"), std::string::npos) << line;
    EXPECT_NE(line.find("cu0 115.48 65C"), std::string::npos) << line;
    EXPECT_NE(line.find("cu1 paused"), std::string::npos) << line;

    pool.connected = false;
    EXPECT_EQ(strip(summaryLine(twoGpus(), pool)).find("Not connected"), 0u);
}

TEST(console, stats_table)
{
    PoolStatus pool;
    pool.connected = true;
    pool.host = "pool.example.com:3333";
    pool.difficulty = 4294967296.0;
    pool.epoch = 83;
    pool.block = 60268;  // 212 blocks before epoch 84
    auto lines = statsTable(twoGpus(), {{"NVIDIA GeForce RTX 4090", ""}, {"RTX 3070", "Overheating"}}, pool);
    ASSERT_EQ(lines.size(), 5u);  // header, 2 GPUs, total, footer
    for (auto& l : lines)
        l = strip(l);
    EXPECT_NE(lines[0].find("Mh/s"), std::string::npos);
    EXPECT_NE(lines[1].find("NVIDIA GeForce RTX 4090"), std::string::npos);
    EXPECT_NE(lines[1].find("115.48"), std::string::npos);
    EXPECT_NE(lines[1].find("280W"), std::string::npos);
    EXPECT_NE(lines[1].find("412"), std::string::npos) << lines[1];  // kh/W
    EXPECT_NE(lines[2].find("paused"), std::string::npos);
    EXPECT_NE(lines[2].find("Overheating"), std::string::npos);
    EXPECT_EQ(lines[3].find("Total"), 0u);
    EXPECT_NE(lines[4].find("pool.example.com:3333"), std::string::npos);
    EXPECT_NE(lines[4].find("epoch 83 (next in 212 blocks, ~35 h)"), std::string::npos) << lines[4];
}
