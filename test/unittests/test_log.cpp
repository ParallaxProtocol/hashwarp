// Log levels and the log file sink

#include <libdevcore/Log.h>

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace fs = boost::filesystem;

namespace
{
std::string readFile(const fs::path& _p)
{
    std::ifstream in(_p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct LogFileTest : public ::testing::Test
{
    fs::path dir;
    dev::LogLevel savedLevel;

    void SetUp() override
    {
        dir = fs::temp_directory_path() / fs::unique_path("hashwarp-log-%%%%-%%%%");
        fs::create_directories(dir);
        savedLevel = g_logLevel;
    }
    void TearDown() override
    {
        g_logLevel = savedLevel;
        dev::setLogFile("");  // release the test file (Windows can't remove open files)
        fs::remove_all(dir);
    }
};
}  // namespace

TEST(log, parse_levels)
{
    EXPECT_EQ(dev::parseLogLevel("error"), dev::LogLevel::Error);
    EXPECT_EQ(dev::parseLogLevel("warning"), dev::LogLevel::Warn);
    EXPECT_EQ(dev::parseLogLevel("trace"), dev::LogLevel::Trace);
    EXPECT_THROW(dev::parseLogLevel("loud"), std::invalid_argument);
    EXPECT_STREQ(dev::logLevelName(dev::LogLevel::Debug), "debug");
}

TEST_F(LogFileTest, writes_plain_lines_and_filters_levels)
{
    auto path = dir / "hashwarp.log";
    dev::setLogFile(path.string());
    g_logLevel = dev::LogLevel::Info;

    cnote << EthWhite "colored note" EthReset;
    cdebug << "hidden debug line";
    g_logLevel = dev::LogLevel::Debug;
    cdebug << "visible debug line";

    std::string content = readFile(path);
    EXPECT_NE(content.find("colored note"), std::string::npos);
    EXPECT_EQ(content.find('\x1b'), std::string::npos) << "colors must be stripped";
    EXPECT_EQ(content.find("hidden debug line"), std::string::npos);
    EXPECT_NE(content.find("visible debug line"), std::string::npos);
    // Full date in the file
    EXPECT_EQ(content[4], '-');
    EXPECT_EQ(content[7], '-');
}

TEST_F(LogFileTest, rotates_when_full)
{
    auto path = dir / "hashwarp.log";
    dev::setLogFile(path.string(), 2000, 2);
    for (int i = 0; i < 200; i++)
        cnote << "line " << i << " padding padding padding padding";

    EXPECT_TRUE(fs::exists(path));
    EXPECT_TRUE(fs::exists(dir / "hashwarp.log.1"));
    EXPECT_TRUE(fs::exists(dir / "hashwarp.log.2"));
    EXPECT_FALSE(fs::exists(dir / "hashwarp.log.3"));
    EXPECT_LE(fs::file_size(dir / "hashwarp.log.1"), 2000u + 200u);
    EXPECT_NE(readFile(path).find("line 199"), std::string::npos);
}

TEST_F(LogFileTest, json_lines)
{
    auto path = dir / "hashwarp.log";
    dev::setLogFile(path.string());
    g_logFormat = dev::LogFormat::Json;
    clogs(dev::WarnChannel, "share").with("gpu", 1).with("ms", 42.5).with("stale", false)
        << EthRed "✘ rejected" EthReset " \"low\"\tdifficulty";
    g_logFormat = dev::LogFormat::Text;

    // Not raw strings: MSVC can't take them as macro arguments
    const std::string message =
        "\"level\":\"warn\",\"source\":\"share\",\"msg\":\"✘ rejected \\\"low\\\"\\tdifficulty\"";
    const std::string fields = ",\"gpu\":1,\"ms\":42.5,\"stale\":false}";
    std::string line = readFile(path);
    EXPECT_EQ(line.compare(0, 9, "{\"time\":\""), 0) << line;
    EXPECT_NE(line.find(message), std::string::npos) << line;
    EXPECT_NE(line.find(fields), std::string::npos) << line;
    EXPECT_EQ(line.find('\x1b'), std::string::npos);
}

TEST_F(LogFileTest, text_columns)
{
    auto path = dir / "hashwarp.log";
    dev::setLogFile(path.string());
    clogs(dev::NoteChannel, "pool") << "Connected";
    std::string line = readFile(path);
    // 2026-10-18T21:44:32.123-03:00 INFO  pool   Connected
    EXPECT_EQ(line[10], 'T') << line;
    EXPECT_NE(line.find(" INFO  pool   Connected"), std::string::npos) << line;
}

TEST(log, sources_and_ascii)
{
    std::string name = dev::getThreadName();
    dev::setThreadName("cuda-3");
    EXPECT_EQ(dev::threadLogSource(), "gpu3");
    dev::setThreadName("cl-12");
    EXPECT_EQ(dev::threadLogSource(), "gpu12");
    dev::setThreadName("cpu-0");
    EXPECT_EQ(dev::threadLogSource(), "cpu0");
    dev::setThreadName("sim");
    EXPECT_EQ(dev::threadLogSource(), "pool");
    dev::setThreadName("watchdog");
    EXPECT_EQ(dev::threadLogSource(), "miner");
    dev::setThreadName(name.c_str());
    EXPECT_EQ(dev::plainText(EthLime "✔ accepted" EthReset " · 54°C → ok", false),
        "+ accepted | 54C -> ok");
    EXPECT_EQ(dev::plainText("✔ 54°C"), "✔ 54°C");
}
