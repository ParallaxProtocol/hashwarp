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
